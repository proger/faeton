import AppKit
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers
import Darwin

struct LaunchConfig {
    let inputFileURL: URL?
    let outputFileURL: URL?
}

final class OverlayApp: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private let defaultInputPlaceholder = "ask:"
    private let contentSidePad: CGFloat = 8
    private let contentBottomPad: CGFloat = 10
    private let transcriptInsetX: CGFloat = 12
    private let transcriptInsetY: CGFloat = 10
    private let transcriptLinePadding: CGFloat = 3
    private let inputInnerLeftApprox: CGFloat = 5
    private let subBaseURL = URL(string: "https://approximate.fit/sub")!
    private let pubURL = URL(string: "https://approximate.fit/pub")!
    private let config: LaunchConfig
    private let panel = NSPanel(
        contentRect: NSRect(x: 0, y: 0, width: 620, height: 240),
        styleMask: [.titled, .resizable, .fullSizeContentView],
        backing: .buffered,
        defer: false
    )
    private let scrollView = NSScrollView()
    private let transcriptView = NSTextView()
    private let inputField = NSTextField(string: "")
    private var captureTimer: Timer?
    private var fileTimer: Timer?
    private var subTask: Task<Void, Never>?
    private var keyMonitor: Any?
    private var lastText = ""
    private var mainFontSize: CGFloat = 14
    private let minMainFontSize: CGFloat = 10
    private let maxMainFontSize: CGFloat = 42
    private lazy var timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    init(config: LaunchConfig) {
        self.config = config
        super.init()
    }

    private func log(_ msg: String) {
        print(msg)
        fflush(stdout)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        setupMainMenu()
        setupWindow()
        layout(resetWindowFrame: true)
        setDisplayText("Recording active.")
        panel.makeKeyAndOrderFront(nil)
        focusInputField()
        if let inputFileURL = config.inputFileURL {
            log("Faeton HUD started in single-player mode. I read overlay text from \(inputFileURL.path). Screenshot uploads are disabled.")
            refreshTextFromFile()
            fileTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
                self?.refreshTextFromFile()
            }
        } else {
            log("Faeton HUD started in multiplayer mode. I subscribe to live text updates from \(subBaseURL.absoluteString)/0 and upload screenshots every 5 seconds.")
            startSubscribeLoop()
            captureTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
                guard let self else { return }
                Task {
                    await self.captureAndUploadIfActive()
                }
            }
        }
        installKeyMonitor()
    }

    private func setupWindow() {
        panel.delegate = self
        panel.isReleasedWhenClosed = false
        panel.isFloatingPanel = true
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = false
        panel.hasShadow = true
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isMovableByWindowBackground = true
        panel.standardWindowButton(.closeButton)?.isHidden = true
        panel.standardWindowButton(.miniaturizeButton)?.isHidden = true
        panel.standardWindowButton(.zoomButton)?.isHidden = true
        panel.backgroundColor = NSColor.black.withAlphaComponent(0.80)
        panel.isOpaque = false

        scrollView.drawsBackground = false
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = false
        scrollView.autohidesScrollers = true
        scrollView.borderType = .noBorder

        transcriptView.drawsBackground = false
        transcriptView.isEditable = false
        transcriptView.isSelectable = true
        transcriptView.textContainerInset = NSSize(width: transcriptInsetX, height: transcriptInsetY)
        transcriptView.font = mainTextFont()
        transcriptView.textColor = .white
        transcriptView.isHorizontallyResizable = false
        transcriptView.isVerticallyResizable = true
        transcriptView.autoresizingMask = [.width]
        transcriptView.textContainer?.lineFragmentPadding = transcriptLinePadding
        transcriptView.textContainer?.widthTracksTextView = true
        transcriptView.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
        transcriptView.textContainer?.heightTracksTextView = false
        scrollView.documentView = transcriptView

        inputField.font = mainTextFont()
        inputField.placeholderString = defaultInputPlaceholder
        inputField.isBezeled = true
        inputField.bezelStyle = .roundedBezel
        inputField.focusRingType = .none
        inputField.target = self
        inputField.action = #selector(handleInputSubmit(_:))

        let container = NSView(frame: panel.contentView!.bounds)
        container.wantsLayer = true
        container.layer?.backgroundColor = NSColor.clear.cgColor
        panel.contentView = container
        scrollView.autoresizingMask = [.width, .height]
        inputField.autoresizingMask = [.width, .maxYMargin]
        container.addSubview(scrollView)
        container.addSubview(inputField)
        applyCurrentFonts()
    }

    private func setupMainMenu() {
        let mainMenu = NSMenu()
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)

        let appMenu = NSMenu()
        let appName = ProcessInfo.processInfo.processName
        let quitTitle = "Quit \(appName)"
        appMenu.addItem(
            withTitle: quitTitle,
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q"
        )
        appItem.submenu = appMenu
        NSApp.mainMenu = mainMenu
    }

    private func updateText(_ text: String, eventID: String? = nil) {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let body = cleaned.isEmpty ? "Recording active." : cleaned
        if config.inputFileURL != nil {
            if body == lastText { return }
            lastText = body
            setDisplayText(body)
        } else {
            appendLogEntry(body, eventID: eventID)
        }
    }

    private func layout(resetWindowFrame: Bool = false) {
        let pad: CGFloat = 24
        let width: CGFloat = 620
        let height: CGFloat = 280

        if resetWindowFrame {
            let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
            let x = screenFrame.minX + pad
            let y = screenFrame.minY + ((screenFrame.height - height) / 2)
            panel.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true)
        }
        let bounds = panel.contentView?.bounds ?? NSRect(x: 0, y: 0, width: width, height: height)
        let gap: CGFloat = 6
        let inputHeight: CGFloat = max(22, ceil(mainFontSize + 10))
        let transcriptTextLeft = contentSidePad + transcriptInsetX + transcriptLinePadding
        let inputFrameX = max(contentSidePad, transcriptTextLeft - inputInnerLeftApprox)
        inputField.frame = NSRect(
            x: inputFrameX,
            y: contentBottomPad,
            width: max(40, bounds.width - inputFrameX - contentSidePad),
            height: inputHeight
        )
        let scrollY = contentBottomPad + inputHeight + gap
        scrollView.frame = NSRect(
            x: contentSidePad,
            y: scrollY,
            width: max(40, bounds.width - (contentSidePad * 2)),
            height: max(40, bounds.height - scrollY - contentBottomPad)
        )
    }

    private func setDisplayText(_ text: String) {
        let body = text.isEmpty ? "Recording active." : text
        let renderedBody = body == "Recording active." ? "Recording active.\n" : body
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byWordWrapping

        let full = NSMutableAttributedString()
        full.append(NSAttributedString(
            string: renderedBody,
            attributes: [
                .font: mainTextFont(),
                .foregroundColor: NSColor.white,
                .paragraphStyle: paragraph
            ]
        ))
        transcriptView.textStorage?.setAttributedString(full)
        let end = NSRange(location: transcriptView.string.count, length: 0)
        transcriptView.scrollRangeToVisible(end)
    }

    private func singlePlayerOutputFileURL() -> URL? {
        if let outputFileURL = config.outputFileURL {
            return outputFileURL
        }
        if let inputFileURL = config.inputFileURL {
            return inputFileURL.deletingLastPathComponent().appendingPathComponent("_pub.txt")
        }
        return nil
    }

    @objc
    private func handleInputSubmit(_ sender: NSTextField) {
        let text = sender.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        sender.stringValue = ""
        if config.inputFileURL != nil {
            writePubTextFile(text)
        } else {
            postPub(text)
        }
    }

    private func writePubTextFile(_ text: String) {
        guard let fileURL = singlePlayerOutputFileURL() else {
            log("pub text error: missing output/input file path")
            return
        }
        do {
            try FileManager.default.createDirectory(
                at: fileURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let line = text + "\n"
            let data = Data(line.utf8)
            if FileManager.default.fileExists(atPath: fileURL.path) {
                let handle = try FileHandle(forWritingTo: fileURL)
                try handle.seekToEnd()
                try handle.write(contentsOf: data)
                try handle.close()
            } else {
                try data.write(to: fileURL, options: .atomic)
            }
            log("pub text file \(fileURL.path)")
        } catch {
            log("pub text error: \(error.localizedDescription)")
        }
    }

    private func postPub(_ text: String) {
        var req = URLRequest(url: pubURL)
        req.httpMethod = "POST"
        req.setValue("text/plain", forHTTPHeaderField: "Content-Type")
        req.httpBody = Data(text.utf8)
        URLSession.shared.dataTask(with: req) { [weak self] data, response, error in
            guard let self else { return }
            if let error {
                self.log("pub text error: \(error.localizedDescription)")
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            if let data, let body = String(data: data, encoding: .utf8), !body.isEmpty {
                self.log("pub text status=\(code) \(body.trimmingCharacters(in: .whitespacesAndNewlines))")
            } else {
                self.log("pub text status=\(code)")
            }
        }.resume()
    }

    private func appendLogEntry(_ text: String, eventID: String?) {
        let tsString = humanTime(from: eventID) ?? "--:--:--"
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineBreakMode = .byWordWrapping

        let line = NSMutableAttributedString()
        line.append(NSAttributedString(
            string: "[\(tsString)] ",
            attributes: [
                .font: timestampFont(),
                .foregroundColor: timestampColor(tsString),
                .paragraphStyle: paragraph
            ]
        ))
        line.append(NSAttributedString(
            string: text + "\n",
            attributes: [
                .font: mainTextFont(),
                .foregroundColor: NSColor.white,
                .paragraphStyle: paragraph
            ]
        ))

        transcriptView.textStorage?.append(line)
        let end = NSRange(location: transcriptView.string.count, length: 0)
        transcriptView.scrollRangeToVisible(end)
    }

    private func humanTime(from eventID: String?) -> String? {
        guard let eventID, let seconds = Double(eventID) else { return nil }
        let d = Date(timeIntervalSince1970: seconds)
        return timeFormatter.string(from: d)
    }

    private func timestampColor(_ s: String) -> NSColor {
        let hue = CGFloat(abs(s.hashValue % 360)) / 360.0
        return NSColor(calibratedHue: hue, saturation: 0.72, brightness: 0.95, alpha: 1.0)
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
        var eventId = ""
        var eventType = ""
        var eventText = ""
        var hasText = false

        func flushCurrentEvent() async -> Bool {
            if eventId.isEmpty && eventType.isEmpty && !hasText {
                return false
            }
            log("sub event id=\(eventId.isEmpty ? "-" : eventId) type=\(eventType.isEmpty ? "-" : eventType)")
            if hasText {
                let text = eventText.replacingOccurrences(of: "\\n", with: "\n")
                log("sub text \(text)")
                let currentEventID = eventId
                await MainActor.run { updateText(text, eventID: currentEventID) }
                if stopAfterFirstText {
                    return true
                }
            }
            eventId = ""
            eventType = ""
            eventText = ""
            hasText = false
            return false
        }

        do {
            var req = URLRequest(url: url)
            req.httpMethod = "GET"
            req.setValue("text/event-stream", forHTTPHeaderField: "Accept")
            req.setValue("no-cache", forHTTPHeaderField: "Cache-Control")
            log("sub open \(url.absoluteString)")
            let (bytes, _) = try await URLSession.shared.bytes(for: req)

            for try await raw in bytes.lines {
                if Task.isCancelled { return }
                let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
                if line.hasPrefix("id:") {
                    if await flushCurrentEvent() {
                        return
                    }
                    eventId = String(line.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                    continue
                }
                if line.isEmpty {
                    if await flushCurrentEvent() {
                        return
                    }
                    continue
                }
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
                    if await flushCurrentEvent() {
                        return
                    }
                }
            }
            _ = await flushCurrentEvent()
        } catch {
            _ = await flushCurrentEvent()
            log("sub error \(error.localizedDescription)")
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        fileTimer?.invalidate()
        fileTimer = nil
        captureTimer?.invalidate()
        captureTimer = nil
        subTask?.cancel()
        subTask = nil
        if let keyMonitor {
            NSEvent.removeMonitor(keyMonitor)
            self.keyMonitor = nil
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        panel.makeKeyAndOrderFront(nil)
        focusInputField()
    }

    func windowDidBecomeKey(_ notification: Notification) {
        focusInputField()
    }

    func windowDidBecomeMain(_ notification: Notification) {
        focusInputField()
    }

    private func mainTextFont() -> NSFont {
        NSFont(name: "Menlo", size: mainFontSize) ?? NSFont.monospacedSystemFont(ofSize: mainFontSize, weight: .regular)
    }

    private func timestampFont() -> NSFont {
        let size = max(8, mainFontSize - 1)
        return NSFont(name: "Menlo-Bold", size: size) ?? NSFont.monospacedSystemFont(ofSize: size, weight: .semibold)
    }

    private func applyCurrentFonts() {
        transcriptView.font = mainTextFont()
        inputField.font = mainTextFont()
    }

    private func restyleTranscriptForCurrentFontSize() {
        guard let storage = transcriptView.textStorage else {
            return
        }
        let full = NSRange(location: 0, length: storage.length)
        if full.length == 0 {
            return
        }
        storage.beginEditing()
        storage.enumerateAttributes(in: full, options: []) { attrs, range, _ in
            var updated = attrs
            let color = attrs[.foregroundColor] as? NSColor
            let isTimestamp = (color != nil) && (color != NSColor.white)
            updated[.font] = isTimestamp ? timestampFont() : mainTextFont()
            storage.setAttributes(updated, range: range)
        }
        storage.endEditing()
    }

    private func adjustMainFontSize(by delta: CGFloat) {
        let next = min(maxMainFontSize, max(minMainFontSize, mainFontSize + delta))
        if next == mainFontSize {
            return
        }
        mainFontSize = next
        applyCurrentFonts()
        restyleTranscriptForCurrentFontSize()
        layout()
        scrollTranscriptToBottom()
    }

    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            guard let self else { return event }
            guard event.modifierFlags.intersection(.deviceIndependentFlagsMask).contains(.command) else {
                return event
            }
            let chars = event.charactersIgnoringModifiers ?? ""
            if chars == "=" || chars == "+" || event.keyCode == 69 {
                self.adjustMainFontSize(by: 1)
                return nil
            }
            if chars == "-" || chars == "_" || event.keyCode == 78 {
                self.adjustMainFontSize(by: -1)
                return nil
            }
            return event
        }
    }

    private func focusInputField() {
        panel.makeFirstResponder(inputField)
    }

    private func scrollTranscriptToBottom() {
        let end = NSRange(location: transcriptView.string.count, length: 0)
        transcriptView.scrollRangeToVisible(end)
    }

    private func refreshTextFromFile() {
        guard let inputFileURL = config.inputFileURL else { return }
        guard let text = try? String(contentsOf: inputFileURL, encoding: .utf8) else { return }
        let parsed = parseTextFileContent(text)
        inputField.placeholderString = parsed.metaPlaceholder
        updateText(parsed.body)
    }

    private func parseTextFileContent(_ text: String) -> (body: String, metaPlaceholder: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            return ("", defaultInputPlaceholder)
        }

        guard let lastNewline = trimmed.lastIndex(of: "\n") else {
            return (trimmed, defaultInputPlaceholder)
        }

        let candidateMeta = trimmed[trimmed.index(after: lastNewline)...]
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if candidateMeta.lowercased().hasPrefix("meta:") {
            let body = trimmed[..<lastNewline].trimmingCharacters(in: .whitespacesAndNewlines)
            return (body, candidateMeta)
        }

        return (trimmed, defaultInputPlaceholder)
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
        config.width = max(1, Int(scDisplay.width) / 2)
        config.height = max(1, Int(scDisplay.height) / 2)
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
        URLSession.shared.uploadTask(with: req, from: body) { _, response, error in
            if let error {
                print("pub png error \(filename) \(error.localizedDescription)")
                fflush(stdout)
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? -1
            print("pub png done \(filename) status=\(code)")
            fflush(stdout)
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
var inputFileURL: URL?
var outputFileURL: URL?
var parseErrors: [String] = []

func printUsage() {
    let usage = """
    Usage: faeton [-i <input-file>] [-o <output-file>]

      -i <input-file>   Read overlay text from a local file (single-player mode)
      -o <output-file>  Append 'ask:' submissions to this local file
      (no -i)           Multiplayer mode: read live updates from https://approximate.fit/sub/0
      -h, --help        Show this help
    """
    fputs(usage + "\n", stderr)
}

var i = 1
while i < CommandLine.arguments.count {
    let arg = CommandLine.arguments[i]
    if arg == "-h" || arg == "--help" {
        printUsage()
        exit(0)
    }
    if arg == "-i", i + 1 < CommandLine.arguments.count {
        inputFileURL = URL(fileURLWithPath: CommandLine.arguments[i + 1])
        i += 2
        continue
    } else if arg == "-i" {
        parseErrors.append("missing value for -i")
        break
    }
    if arg == "-o", i + 1 < CommandLine.arguments.count {
        outputFileURL = URL(fileURLWithPath: CommandLine.arguments[i + 1])
        i += 2
        continue
    } else if arg == "-o" {
        parseErrors.append("missing value for -o")
        break
    }
    parseErrors.append("unrecognized argument: \(arg)")
    i += 1
}
if !parseErrors.isEmpty {
    for err in parseErrors {
        fputs("error: \(err)\n", stderr)
    }
    printUsage()
    exit(2)
}
let delegate = OverlayApp(config: LaunchConfig(inputFileURL: inputFileURL, outputFileURL: outputFileURL))
app.delegate = delegate
app.run()
