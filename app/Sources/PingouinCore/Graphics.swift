import Foundation

// Venus (Vulkan) sizing, ported from omacvm's Graphics.swift (MIT, Gilles Goetsch).

/// The virtio-gpu memory window for Venus and, on M1/M2, the PCI window it needs.
public struct VenusPlan: Equatable, Sendable {
    /// virtio-gpu's hostmem (MB): how much Vulkan memory the guest can map.
    public var hostmemMB: Int
    /// -machine highmem-mmio-size (GB), only on Macs whose VMs get under 40 bits.
    public var highWindowGB: Int?

    /// What this Mac has beyond the VM's own memory and macOS's reserve (4 GB
    /// up to 16 GB, 6 GB up to 36 GB, 8 GB above), as a power of two from 4 to
    /// 32 GB. QEMU virt's 512 GB PCI window above 512 GB exists only with a
    /// 40-bit VM address space (M3 and newer); M1 and M2 give 36 bits, so the
    /// window goes right above RAM (qemu-virt-small-high-window.patch) and the
    /// BAR takes at most half of it. Without a window, 256 MB is all that fits.
    public init(macMemoryGB: Int, vmMemoryGB: Int, ipaBits: Int?) {
        let usual: Int = {
            let reserve = macMemoryGB <= 16 ? 4 : macMemoryGB <= 36 ? 6 : 8
            let free = min(max(macMemoryGB - vmMemoryGB - reserve, 4), 32)
            var p = 1
            while p * 2 <= free { p *= 2 }
            return p * 1024
        }()
        if let b = ipaBits, b < 40 {
            highWindowGB = VenusPlan.highWindowGB(vmMemoryGB: vmMemoryGB, ipaBits: b)
            hostmemMB = highWindowGB.map { min(usual, $0 * 1024 / 2) } ?? 256
        } else {
            highWindowGB = nil
            hostmemMB = usual
        }
    }

    /// The largest power of two up to 16 GB that fits: QEMU puts the window on
    /// its own size's boundary after RAM (from 1 GB) and the other high regions,
    /// so it starts at vmMemoryGB + 3 GB at the latest.
    static func highWindowGB(vmMemoryGB: Int, ipaBits b: Int) -> Int? {
        guard b > 30 else { return nil }
        let top = 1 << (b - 30), start = vmMemoryGB + 3
        var w = 16
        while w >= 1 {
            if (start + w - 1) / w * w + w <= top { return w }
            w /= 2
        }
        return nil
    }

    /// This Mac, for a VM of `memoryMiB`.
    public static func forThisMac(memoryMiB: Int) -> VenusPlan {
        VenusPlan(macMemoryGB: Int(ProcessInfo.processInfo.physicalMemory >> 30),
                  vmMemoryGB: (memoryMiB + 1023) / 1024, ipaBits: vmAddressBits)
    }

    /// The VM address space Hypervisor.framework gives (bits), nil if unknown.
    public static var vmAddressBits: Int? {
        let sizes = ["kern.hv.ipa_size_16k", "kern.hv.ipa_size_4k"].compactMap { name -> UInt64? in
            var v: UInt64 = 0
            var n = MemoryLayout<UInt64>.size
            return sysctlbyname(name, &v, &n, nil, 0) == 0 && v > 0 ? v : nil
        }
        guard let s = sizes.min() else { return nil }
        return UInt64.bitWidth - 1 - s.leadingZeroBitCount
    }
}
