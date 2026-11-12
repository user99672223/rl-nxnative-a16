// The diagnostic panel: what the game is doing on this device, and the settings that change it while it runs. It fills the phone's screen while a TV or monitor shows the picture
// and a controller is connected, and is an overlay at the side otherwise (front_ios.m dm_diag_show; the in-game menu's Diagnostics row toggles it).
//   Figures, four times a second, from the host's performance record (dm/host/hle_perf.c): frames per second, the frame-time graph, CPU and GPU time per frame, memory in use against
//   what iOS allows this app (its footprint plus os_proc_available_memory), the thermal state, the size the game renders and the size it is shown at.
//   Controls: every launcher setting the running game takes at once (dm_setting_live), saved as the settings screen saves them (LauncherModel.choose).
//   Export performance log: the whole session as a CSV (a row per frame) and a JSON (device, settings, every change made while playing, a summary of each stretch between changes),
//   written to Documents/perf and handed to the share sheet, so the figures of a device this project could not test on can be sent back.
import SwiftUI
import UIKit
import GameController

final class DiagModel: ObservableObject {
    @Published var sum = dm_perf_sum()
    @Published var frameMs: [Float] = []
    @Published var gpuMs: [Float] = []
    @Published var memUse: UInt64 = 0
    @Published var memPeak: UInt64 = 0
    @Published var memBudget: UInt64 = 0
    @Published var memAvail: UInt64 = 0
    @Published var memMetal: UInt64 = 0
    @Published var thermal = 0
    @Published var exported: [URL] = []
    @Published var exportFailed = false
    @Published var fullScreen = false
    private var timer: Timer?
    static let graphFrames = 240

    func start() {
        guard timer == nil else { return }
        tick()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.tick() }
    }
    func stop() { timer?.invalidate(); timer = nil }
    func tick() {
        var s = dm_perf_sum(); dm_perf_now(&s); sum = s
        var rows = [dm_perf_row](repeating: dm_perf_row(), count: Self.graphFrames)
        let n = rows.withUnsafeMutableBufferPointer { Int(dm_perf_recent($0.baseAddress, Int32(Self.graphFrames))) }
        frameMs = rows.prefix(n).map { $0.frame_ms }
        gpuMs = rows.prefix(n).map { $0.gpu_ms }
        dm_mem_now(&memUse, &memPeak, &memBudget, &memAvail, &memMetal)
        thermal = ProcessInfo.processInfo.thermalState.rawValue
    }
    /// What iOS allows this app: its footprint plus what the system says is left; the budget switch's figure where the system gives none (the Simulator, a Mac)
    var memLimit: UInt64 { memAvail > 0 ? memUse + memAvail : memBudget }
    var thermalText: String { ["Nominal", "Fair", "Serious", "Critical"][min(max(thermal, 0), 3)] }

    /// The session as files in Documents/perf; nil when they could not be written
    func export(settings: [String: String]) -> [URL]? {
        let fm = FileManager.default
        guard let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first else { return nil }
        let dir = docs.appendingPathComponent("perf", isDirectory: true)
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.dateFormat = "yyyyMMdd-HHmmss"
        let base = dir.appendingPathComponent("perf-" + f.string(from: Date()))
        var sys = utsname(); uname(&sys)
        let machine = withUnsafePointer(to: &sys.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) } }
        let screen = UIScreen.main
        let info: [String: Any] = [
            "device": machine, "system": UIDevice.current.systemName + " " + UIDevice.current.systemVersion,
            "app": (Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String) ?? "", "app_version": (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "",
            "physical_memory_mb": ProcessInfo.processInfo.physicalMemory >> 20, "low_power_mode": ProcessInfo.processInfo.isLowPowerModeEnabled,
            "phone_screen": [Int(screen.nativeBounds.height), Int(screen.nativeBounds.width)], "phone_refresh_hz": screen.maximumFramesPerSecond,
            "external_display": dm_tv_connected(), "controllers": GCController.controllers().map { $0.vendorName ?? "controller" },
            "exported": ISO8601DateFormatter().string(from: Date()), "settings": settings,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: info, options: [.sortedKeys]), let json = String(data: data, encoding: .utf8) else { return nil }
        guard dm_perf_export(base.path, json) == 0 else { return nil }
        return [base.appendingPathExtension("json"), base.appendingPathExtension("csv")]
    }
}

/// The last frames' times as bars, with the 60 and 30 fps lines; the GPU's part of each bar darker
struct FrameGraph: View {
    let frame: [Float], gpu: [Float]
    var body: some View {
        Canvas { ctx, size in
            let top: CGFloat = 40   // ms at the top edge
            func y(_ ms: CGFloat) -> CGFloat { size.height * (1 - min(ms, top) / top) }
            for (ms, label) in [(CGFloat(16.67), "60"), (CGFloat(33.33), "30")] {
                var p = Path(); p.move(to: CGPoint(x: 0, y: y(ms))); p.addLine(to: CGPoint(x: size.width, y: y(ms)))
                ctx.stroke(p, with: .color(.white.opacity(0.35)), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                ctx.draw(Text(label).font(.system(size: 9)).foregroundColor(.white.opacity(0.6)), at: CGPoint(x: 10, y: y(ms) - 6))
            }
            let n = DiagModel.graphFrames, w = size.width / CGFloat(n), off = n - frame.count
            for (i, ms) in frame.enumerated() {
                let x = CGFloat(off + i) * w, v = CGFloat(ms)
                let c: Color = v > 34 ? .red : v > 17.5 ? .orange : .green
                ctx.fill(Path(CGRect(x: x, y: y(v), width: max(w - 0.5, 0.5), height: size.height - y(v))), with: .color(c.opacity(0.85)))
                if i < gpu.count, gpu[i] > 0 { let g = CGFloat(gpu[i]); ctx.fill(Path(CGRect(x: x, y: y(g), width: max(w - 0.5, 0.5), height: size.height - y(g))), with: .color(.black.opacity(0.35))) }
            }
        }
        .frame(height: 90)
        .background(Color.black.opacity(0.35))
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .accessibilityLabel("Frame time graph")
    }
}

private struct Figure: View {
    let title: String, value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption2).foregroundColor(.secondary)
            Text(value).font(.system(.body, design: .monospaced).weight(.semibold)).minimumScaleFactor(0.6).lineLimit(1)
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController { UIActivityViewController(activityItems: items, applicationActivities: nil) }
    func updateUIViewController(_ c: UIActivityViewController, context: Context) {}
}

struct DiagPanelView: View {
    @ObservedObject var diag: DiagModel
    @ObservedObject var settings: LauncherModel
    let hide: () -> Void
    @State private var sharing = false

    private func ms(_ v: Float) -> String { v < 0 ? "–" : String(format: "%.1f ms", v) }
    private func mb(_ v: UInt64) -> String { String(format: "%.0f MB", Double(v) / 1048576) }
    private var liveRows: [(String, [Setting])] {
        settings.sections.compactMap { sec in
            let rows = settings.rows(in: sec).filter { dm_setting_live($0.env, nil) }
            return rows.isEmpty ? nil : (sec, rows)
        }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("Diagnostics").font(.headline)
                    Spacer()
                    Button("Hide", action: hide).buttonStyle(.bordered).accessibilityIdentifier("diag.hide")
                }
                HStack(alignment: .firstTextBaseline) {
                    Text(String(format: "%.0f", diag.sum.fps)).font(.system(size: 44, weight: .bold, design: .monospaced)).accessibilityIdentifier("diag.fps")
                    Text("fps").font(.title3).foregroundColor(.secondary)
                    Spacer()
                    VStack(alignment: .trailing) {
                        Text("frame " + ms(diag.sum.frame_p50) + " · 95% " + ms(diag.sum.frame_p95)).font(.caption.monospacedDigit())
                        Text("worst " + ms(diag.sum.frame_max)).font(.caption.monospacedDigit()).foregroundColor(.secondary)
                    }
                }
                FrameGraph(frame: diag.frameMs, gpu: diag.gpuMs)
                HStack {
                    Figure(title: "CPU per frame", value: ms(diag.sum.cpu_ms))
                    Figure(title: "GPU per frame", value: ms(diag.sum.gpu_ms))
                    Figure(title: "Draws / passes", value: String(format: "%.0f / %.0f", diag.sum.draws, diag.sum.passes))
                }
                HStack {
                    Figure(title: "Game renders", value: diag.sum.render_w > 0 ? "\(diag.sum.render_w) × \(diag.sum.render_h)" : "–")
                    Figure(title: "Shown at", value: diag.sum.out_w > 0 ? "\(diag.sum.out_w) × \(diag.sum.out_h), \(dm_display_hz()) Hz" : "\(dm_display_hz()) Hz")
                    Figure(title: "Thermal state", value: diag.thermalText)
                }
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text("Memory").font(.caption2).foregroundColor(.secondary)
                        Spacer()
                        Text(diag.memLimit > 0 ? mb(diag.memUse) + " of " + mb(diag.memLimit) : mb(diag.memUse)).font(.caption.monospacedDigit())
                    }
                    ProgressView(value: diag.memLimit > 0 ? min(Double(diag.memUse) / Double(diag.memLimit), 1) : 0)
                        .tint(diag.memLimit > 0 && Double(diag.memUse) > 0.9 * Double(diag.memLimit) ? .red : .accentColor)
                    Text("peak " + mb(diag.memPeak) + " · Metal " + mb(diag.memMetal)).font(.caption2).foregroundColor(.secondary)
                }.accessibilityIdentifier("diag.memory")

                ForEach(liveRows, id: \.0) { section, rows in
                    Divider()
                    Text(section).font(.subheadline.weight(.semibold))
                    ForEach(rows) { s in DiagRow(setting: s, settings: settings) }
                }

                Divider()
                Button {
                    if let urls = diag.export(settings: settings.values) { diag.exported = urls; diag.exportFailed = false; sharing = true } else { diag.exportFailed = true }
                } label: { Label("Export performance log", systemImage: "square.and.arrow.up").frame(maxWidth: .infinity) }
                .buttonStyle(.borderedProminent).accessibilityIdentifier("diag.export")
                if diag.exportFailed { Text("The log could not be written.").font(.caption).foregroundColor(.red) }
                else if let f = diag.exported.first { Text("Saved in Files › \(Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String ?? "this app") › perf as \(f.deletingPathExtension().lastPathComponent)").font(.caption2).foregroundColor(.secondary) }
                Text("\(dm_perf_rows()) frames recorded this session").font(.caption2).foregroundColor(.secondary)
            }
            .padding(14)
        }
        .background(diag.fullScreen ? AnyShapeStyle(Color(white: 0.08)) : AnyShapeStyle(.ultraThinMaterial))
        .environment(\.colorScheme, .dark)
        .sheet(isPresented: $sharing) { ShareSheet(items: diag.exported) }
        .onAppear { diag.start() }
        .onDisappear { diag.stop() }
    }
}

/// One live setting: a switch, a menu of its options, or a slider that applies when it is let go
private struct DiagRow: View {
    let setting: Setting
    @ObservedObject var settings: LauncherModel
    @State private var slider: Double = 0
    @State private var sliding = false

    private var now: String { settings.values[setting.env] ?? setting.def }
    private var fixed: Bool { settings.fixed.contains(setting.env) }
    private func pick(_ v: String) { settings.choose(setting, v); _ = dm_setting_live(setting.env, v) }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            switch setting.kind {
            case "toggle":
                Toggle(setting.label, isOn: Binding(get: { now == setting.onValue }, set: { pick($0 ? setting.onValue : setting.offValue) }))
            case "choice":
                HStack {
                    Text(setting.label)
                    Spacer()
                    Picker(setting.label, selection: Binding(get: { now }, set: { pick($0) })) {
                        ForEach(setting.options ?? [], id: \.self) { o in Text(o.count > 1 ? o[1] : o[0]).tag(o[0]) }
                    }.pickerStyle(.menu).labelsHidden()
                }
            case "range":
                let lo = setting.min ?? 0, hi = setting.max ?? 1, st = setting.step ?? 0.05
                HStack { Text(setting.label); Spacer(); Text(settings.rangeLabel(setting, sliding ? slider : (Double(now) ?? lo))).font(.caption.monospacedDigit()).foregroundColor(.secondary) }
                Slider(value: Binding(get: { sliding ? slider : (Double(now) ?? lo) }, set: { slider = $0 }), in: lo...hi, step: st) { editing in
                    if editing { slider = Double(now) ?? lo; sliding = true } else { sliding = false; pick(String(format: "%.2f", slider)) }
                }
            default:
                EmptyView()
            }
            if !setting.note.isEmpty { Text(setting.note).font(.caption2).foregroundColor(.secondary) }
            if fixed { Text("Set in env.txt").font(.caption2).foregroundColor(.secondary) }
        }
        .disabled(fixed)
        .accessibilityIdentifier("diag.setting." + setting.env)
    }
}

let diagModel = DiagModel()

/// The panel as a view controller (front_ios.m dm_diag_show adds it to the root view); retained for the caller. fullScreen: it has the phone's screen to itself (the picture is on a TV)
@_cdecl("dm_diag_panel_vc")
public func dmDiagPanelVC(_ fullScreen: Bool) -> UnsafeMutableRawPointer {
    diagModel.fullScreen = fullScreen
    let model = gameMenuModel ?? LauncherModel([])
    let vc = DmHostingController(rootView: DiagPanelView(diag: diagModel, settings: model, hide: { dm_diag_show(false) }))
    vc.view.backgroundColor = .clear
    return Unmanaged.passRetained(vc as UIViewController).toOpaque()
}
@_cdecl("dm_diag_set_fullscreen")
public func dmDiagSetFullScreen(_ on: Bool) { diagModel.fullScreen = on }

/// Export without the button (tests, and DM_CTL-style automation): the first file's path into out; 0 on success
@_cdecl("dm_diag_export_now")
public func dmDiagExportNow(_ out: UnsafeMutablePointer<CChar>?, _ cap: Int32) -> Int32 {
    guard let urls = diagModel.export(settings: gameMenuModel?.values ?? [:]), let first = urls.first else { return -1 }
    diagModel.exported = urls
    if let out = out, cap > 0 { _ = first.path.withCString { strlcpy(out, $0, Int(cap)) } }
    return 0
}
