import CoreGraphics
import Metal
import QuartzCore
import UIKit

/// A UIView backed by a CAMetalLayer. Everything the TV shows goes through
/// this layer; later MoltenVK presents the guest's frames into it.
final class ConsoleView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
}

/// Draws text lines into a BGRA bitmap with CoreGraphics, uploads it to a
/// shared Metal texture, blits that into the layer's drawable, and can read
/// the texture back as PNG for GET /screenshot.
final class ConsoleRenderer {
    let device: MTLDevice
    let commandQueue: MTLCommandQueue
    let width = 1920
    let height = 1080
    private let bytesPerRow: Int
    private let texture: MTLTexture
    private let pixels: UnsafeMutableRawPointer
    private let lock = NSLock()
    private(set) var frameCount = 0
    private weak var layer: CAMetalLayer?

    init?() {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else { return nil }
        device = dev
        commandQueue = q
        bytesPerRow = width * 4
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        desc.usage = [.shaderRead]
        desc.storageMode = .shared
        guard let t = dev.makeTexture(descriptor: desc) else { return nil }
        texture = t
        pixels = UnsafeMutableRawPointer.allocate(byteCount: width * height * 4, alignment: 4096)
        memset(pixels, 0, width * height * 4)
    }

    func attach(to layer: CAMetalLayer) {
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.framebufferOnly = false
        layer.contentsScale = 1
        layer.isOpaque = true
        layer.drawableSize = CGSize(width: width, height: height)
        self.layer = layer
    }

    /// Redraw the console with `lines` and present it. Main thread.
    func render(lines: [String]) {
        lock.lock()
        draw(lines: lines)
        texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                        withBytes: pixels, bytesPerRow: bytesPerRow)
        lock.unlock()
        present()
    }

    private func draw(lines: [String]) {
        let cs = CGColorSpaceCreateDeviceRGB()
        let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        guard let ctx = CGContext(data: pixels, width: width, height: height, bitsPerComponent: 8,
                                  bytesPerRow: bytesPerRow, space: cs, bitmapInfo: info) else { return }
        // UIKit coordinates: origin top-left.
        ctx.translateBy(x: 0, y: CGFloat(height))
        ctx.scaleBy(x: 1, y: -1)
        UIGraphicsPushContext(ctx)

        ctx.setFillColor(red: 0.04, green: 0.05, blue: 0.08, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        ctx.setFillColor(red: 0.16, green: 0.55, blue: 0.95, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: 10))
        ctx.setFillColor(red: 0.95, green: 0.45, blue: 0.10, alpha: 1)
        ctx.fill(CGRect(x: 0, y: height - 10, width: width, height: 10))
        // A moving marker so consecutive screenshots are distinguishable.
        let markerX = (frameCount * 24) % (width - 40)
        ctx.setFillColor(red: 0.2, green: 0.9, blue: 0.4, alpha: 1)
        ctx.fill(CGRect(x: markerX, y: height - 28, width: 40, height: 14))

        let font = UIFont.monospacedSystemFont(ofSize: 28, weight: .regular)
        let para = NSMutableParagraphStyle()
        para.lineBreakMode = .byClipping
        var y: CGFloat = 36
        for line in lines {
            var color = UIColor(white: 0.88, alpha: 1)
            if line.hasPrefix("JIT ok") { color = UIColor(red: 0.3, green: 0.95, blue: 0.4, alpha: 1) }
            else if line.contains("FAIL") || line.contains("crash:") { color = UIColor(red: 1.0, green: 0.35, blue: 0.3, alpha: 1) }
            else if line.hasPrefix("MEM") || line.hasPrefix("VA") { color = UIColor(red: 0.55, green: 0.8, blue: 1.0, alpha: 1) }
            else if line.hasPrefix("---") { color = UIColor(white: 0.55, alpha: 1) }
            let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color, .paragraphStyle: para]
            (line as NSString).draw(in: CGRect(x: 56, y: y, width: CGFloat(width) - 112, height: 36), withAttributes: attrs)
            y += 34
            if y > CGFloat(height) - 60 { break }
        }
        UIGraphicsPopContext()
    }

    private func present() {
        guard let layer = layer else { return }
        guard let drawable = layer.nextDrawable(),
              let cb = commandQueue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { return }
        let w = min(width, drawable.texture.width)
        let h = min(height, drawable.texture.height)
        blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: w, height: h, depth: 1),
                  to: drawable.texture, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()
        cb.present(drawable)
        cb.commit()
        frameCount += 1
    }

    /// PNG of the texture that was last blitted to the drawable. Any thread.
    func snapshotPNG() -> Data? {
        let count = width * height * 4
        var data = Data(count: count)
        lock.lock()
        data.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) -> Void in
            guard let base = raw.baseAddress else { return }
            texture.getBytes(base, bytesPerRow: bytesPerRow,
                             from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        }
        lock.unlock()
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let provider = CGDataProvider(data: data as CFData),
              let img = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                                bitmapInfo: info, provider: provider, decode: nil,
                                shouldInterpolate: false, intent: .defaultIntent) else { return nil }
        return UIImage(cgImage: img).pngData()
    }
}
