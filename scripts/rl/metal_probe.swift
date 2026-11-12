// Prints what the runner's GPU offers (no game data involved).
import Metal
import Foundation
let devs = MTLCopyAllDevices()
print("devices:", devs.count)
for d in devs {
    print("name:", d.name, "| lowPower:", d.isLowPower, "| headless:", d.isHeadless, "| unified:", d.hasUnifiedMemory)
    print("  recommendedMaxWorkingSetSize MB:", d.recommendedMaxWorkingSetSize >> 20, "| maxBufferLength MB:", d.maxBufferLength >> 20)
    print("  BC:", d.supportsBCTextureCompression, "| ASTC family apple2+:", d.supportsFamily(.apple2), "| apple7:", d.supportsFamily(.apple7), "| apple8:", d.supportsFamily(.apple8), "| apple9:", d.supportsFamily(.apple9), "| mac2:", d.supportsFamily(.mac2), "| metal3:", d.supportsFamily(.metal3))
    if #available(macOS 26.0, *) { print("  metal4:", d.supportsFamily(.metal4)) }
    print("  32bitMSAA:", d.supports32BitMSAA, "| raytracing:", d.supportsRaytracing, "| counterSampling stage:", d.supportsCounterSampling(.atStageBoundary), "| draw:", d.supportsCounterSampling(.atDrawBoundary))
    print("  counterSets:", (d.counterSets ?? []).map { $0.name })
}
print("physical memory GB:", ProcessInfo.processInfo.physicalMemory >> 30, "| cores:", ProcessInfo.processInfo.activeProcessorCount)
