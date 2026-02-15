import AppKit
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers
import Darwin

final class OverlayApp: NSObject, NSApplicationDelegate {
    private let subBaseURL = URL(string: "https://approximate.fit/sub")!
    private let panel = NSPanel(
        contentRect: NSRect(x: 0, y: 0, width: 620, height: 240),
        styleMask: [.borderless, .nonactivatingPanel],
        backing: .buffered,
        defer: false
    )
    private let textField = NSTextField(wrappingLabelWithString: "")
    private let metaField = NSTextField(wrappingLabelWithString: "")
    private var captureTimer: Timer?
    private var subTask: Task<Void, Never>?
    private var lastText = ""

    private func log(_ msg: String) {
        print(msg)
        fflush(stdout)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        setupWindow()
        layout(text: "Recording active.")
        panel.orderFrontRegardless()
        startSubscribeLoop()
        captureTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task {
                await self.captureAndUploadIfActive()
            }
        }
    }

    private func setupWindow() {
        panel.isReleasedWhenClosed = false
        panel.isFloatingPanel = true
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = false
        panel.hasShadow = true
        panel.backgroundColor = NSColor.black.withAlphaComponent(0.80)
        panel.isOpaque = false

        textField.textColor = .white
        textField.font = NSFont(name: "Menlo", size: 16) ?? NSFont.monospacedSystemFont(ofSize: 16, weight: .regular)
        textField.maximumNumberOfLines = 0
        textField.lineBreakMode = .byWordWrapping
        textField.backgroundColor = .clear
        textField.isBezeled = false
        textField.isEditable = false
        textField.isSelectable = false

        metaField.textColor = NSColor.white.withAlphaComponent(0.78)
        metaField.font = NSFont(name: "Menlo", size: 11) ?? NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        metaField.maximumNumberOfLines = 1
        metaField.lineBreakMode = .byTruncatingTail
        metaField.backgroundColor = .clear
        metaField.isBezeled = false
        metaField.isEditable = false
        metaField.isSelectable = false

        let container = NSView(frame: panel.contentView!.bounds)
        container.wantsLayer = true
        container.layer?.backgroundColor = NSColor.clear.cgColor
        panel.contentView = container
        container.addSubview(textField)
        container.addSubview(metaField)
    }

    private func updateText(_ text: String) {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleaned == lastText { return }
        lastText = cleaned
        layout(text: cleaned.isEmpty ? "Recording active." : cleaned)
    }

    private func layout(text: String) {
        let pad: CGFloat = 24
        let innerPadX: CGFloat = 18
        let innerPadY: CGFloat = 14
        let innerGap: CGFloat = 8
        let metaBottom: CGFloat = 10
        let width: CGFloat = 620
        let maxHeight = max(220, NSScreen.main?.frame.height ?? 900 - (pad * 2))

        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        let metaLine: String
        let mainText: String
        if let last = lines.last,
           (last.lowercased().hasPrefix("meta:") || last.lowercased().hasPrefix("step:")) {
            if last.lowercased().hasPrefix("meta:") {
                metaLine = String(last.dropFirst(5)).trimmingCharacters(in: .whitespaces)
            } else {
                metaLine = last
            }
            mainText = lines.dropLast().joined(separator: "\n")
        } else {
            metaLine = ""
            mainText = text
        }

        textField.stringValue = mainText
        metaField.stringValue = metaLine
        textField.preferredMaxLayoutWidth = width - (innerPadX * 2)
        metaField.preferredMaxLayoutWidth = width - (innerPadX * 2)
        let mainFit = textField.fittingSize
        let metaFit = metaLine.isEmpty ? NSSize(width: 0, height: 0) : metaField.fittingSize
        let neededHeight = mainFit.height + (innerPadY * 2) + (metaLine.isEmpty ? 0 : (innerGap + metaFit.height + metaBottom))
        let height = min(max(neededHeight, 220), maxHeight)

        let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let x = screenFrame.maxX - width - pad
        let y = screenFrame.maxY - height - pad

        panel.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true)
        let metaHeight = metaLine.isEmpty ? 0 : metaFit.height
        let textBottom = innerPadY + (metaLine.isEmpty ? 0 : (metaBottom + metaHeight + innerGap))
        textField.frame = NSRect(
            x: innerPadX,
            y: textBottom,
            width: width - (innerPadX * 2),
            height: max(40, height - textBottom - innerPadY)
        )
        metaField.frame = NSRect(
            x: innerPadX,
            y: metaBottom,
            width: width - (innerPadX * 2),
            height: metaHeight
        )
    }

    private func startSubscribeLoop() {
        subTask = Task.detached { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                let liveURL = URL(string: "\(self.subBaseURL.absoluteString)/0") ?? self.subBaseURL
                await self.consumeSSE(url: liveURL, stopAfterFirstText: false)
                self.log("sub reconnecting")
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    private func consumeSSE(url: URL, stopAfterFirstText: Bool) async {
        do {
            var req = URLRequest(url: url)
            req.httpMethod = "GET"
            req.setValue("text/event-stream", forHTTPHeaderField: "Accept")
            req.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
            log("sub open \(url.absoluteString)")
            let (bytes, _) = try await URLSession.shared.bytes(for: req)

            var eventId = ""
            var eventType = ""
            var eventText = ""
            var hasText = false
            for try await raw in bytes.lines {
                if Task.isCancelled { return }
                let line = raw.trimmingCharacters(in: .newlines)
                if line.hasPrefix("id:") {
                    if !eventId.isEmpty || !eventType.isEmpty {
                        log("sub event id=\(eventId.isEmpty ? "-" : eventId) type=\(eventType.isEmpty ? "-" : eventType)")
                    }
                    if hasText {
                        let text = eventText.replacingOccurrences(of: "\\n", with: "\n")
                        log("sub text \(text)")
                        await MainActor.run { updateText(text) }
                        if stopAfterFirstText {
                            return
                        }
                    }
                    eventText = ""
                    hasText = false
                    eventId = String(line.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                    eventType = ""
                    continue
                }
                if line.isEmpty { continue }
                guard line.hasPrefix("data:") else { continue }
                let payload = String(line.dropFirst(5)).trimmingCharacters(in: .whitespaces)
                guard let colon = payload.firstIndex(of: ":") else { continue }
                let key = payload[..<colon].trimmingCharacters(in: .whitespaces)
                let value = payload[payload.index(after: colon)...].trimmingCharacters(in: .whitespaces)
                if key == "type" {
                    eventType = value
                    continue
                }
                if key == "text" {
                    eventText = value
                    hasText = true
                }
            }
        } catch {
            log("sub error \(error.localizedDescription)")
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        captureTimer?.invalidate()
        captureTimer = nil
        subTask?.cancel()
        subTask = nil
    }

    private func isDotaFrontmost() -> Bool {
        guard let app = NSWorkspace.shared.frontmostApplication else {
            return false
        }
        if app.bundleIdentifier == "com.valvesoftware.dota2" {
            return true
        }
        if let exe = app.executableURL?.lastPathComponent.lowercased(), exe.contains("dota2") {
            return true
        }
        if app.localizedName?.lowercased().contains("dota") == true {
            return true
        }
        return false
    }

    private func makeUUIDv1String() -> String {
        var raw: uuid_t = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        uuid_generate_time(&raw)
        let str = UnsafeMutablePointer<CChar>.allocate(capacity: 37)
        defer { str.deallocate() }
        withUnsafePointer(to: &raw) { p in
            p.withMemoryRebound(to: UInt8.self, capacity: 16) { b in
                uuid_unparse_lower(b, str)
            }
        }
        return String(cString: str)
    }

    private func captureAndUploadIfActive() async {
        guard let display = NSScreen.main?.displayID else {
            log("pub png skip: no main display id")
            return
        }
        guard let content = try? await SCShareableContent.current,
              let scDisplay = content.displays.first(where: { $0.displayID == display }) ?? content.displays.first
        else {
            log("pub png skip: SCShareableContent unavailable")
            return
        }
        let filter = SCContentFilter(display: scDisplay, excludingWindows: [])
        let config = SCStreamConfiguration()
        config.width = max(1, Int(scDisplay.width) / 4)
        config.height = max(1, Int(scDisplay.height) / 4)
        config.showsCursor = true
        guard let src = try? await SCScreenshotManager.captureImage(contentFilter: filter, configuration: config) else {
            log("pub png skip: captureImage failed (screen recording permission?)")
            return
        }

        let dstW = max(1, src.width)
        let dstH = max(1, src.height)
        guard let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(
                data: nil,
                width: dstW,
                height: dstH,
                bitsPerComponent: 8,
                bytesPerRow: dstW * 4,
                space: cs,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              ) else {
            log("pub png skip: CGContext init failed")
            return
        }

        ctx.interpolationQuality = .medium
        ctx.draw(src, in: CGRect(x: 0, y: 0, width: dstW, height: dstH))
        guard let scaled = ctx.makeImage() else {
            log("pub png skip: makeImage failed")
            return
        }

        let mutable = CFDataCreateMutable(nil, 0)!
        guard let dest = CGImageDestinationCreateWithData(
            mutable,
            UTType.png.identifier as CFString,
            1,
            nil
        ) else {
            log("pub png skip: CGImageDestinationCreateWithData failed")
            return
        }
        CGImageDestinationAddImage(dest, scaled, nil)
        guard CGImageDestinationFinalize(dest) else {
            log("pub png skip: CGImageDestinationFinalize failed")
            return
        }

        let filename = "\(makeUUIDv1String()).png"
        guard let url = URL(string: "https://approximate.fit/png/\(filename)") else {
            log("pub png skip: invalid upload URL")
            return
        }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("image/png", forHTTPHeaderField: "Content-Type")
        let body = mutable as Data
        log("pub png \(filename) bytes=\(body.count)")
        URLSession.shared.uploadTask(with: req, from: body) { [weak self] _, response, error in
            guard let self else { return }
            if let error {
                self.log("pub png error \(filename) \(error.localizedDescription)")
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            self.log("pub png done \(filename) status=\(code)")
        }.resume()
    }

}

private extension NSScreen {
    var displayID: CGDirectDisplayID? {
        guard let n = deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
            return nil
        }
        return CGDirectDisplayID(n.uint32Value)
    }
}

let app = NSApplication.shared
let delegate = OverlayApp()
app.delegate = delegate
app.run()
