import AppKit
import Foundation

final class OverlayApp: NSObject, NSApplicationDelegate {
    private let textFileURL: URL
    private let panel = NSPanel(
        contentRect: NSRect(x: 0, y: 0, width: 620, height: 240),
        styleMask: [.borderless, .nonactivatingPanel],
        backing: .buffered,
        defer: false
    )
    private let textField = NSTextField(wrappingLabelWithString: "")
    private let metaField = NSTextField(wrappingLabelWithString: "")
    private var timer: Timer?
    private var lastText = ""

    init(textFileURL: URL) {
        self.textFileURL = textFileURL
        super.init()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        setupWindow()
        refreshTextIfNeeded()
        panel.orderFrontRegardless()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.refreshTextIfNeeded()
        }
    }

    private func setupWindow() {
        panel.isReleasedWhenClosed = false
        panel.isFloatingPanel = true
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.hidesOnDeactivate = false
        panel.ignoresMouseEvents = true
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

    private func refreshTextIfNeeded() {
        guard let text = try? String(contentsOf: textFileURL, encoding: .utf8) else {
            return
        }
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if cleaned == lastText {
            return
        }
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
            metaLine = last
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
}

private func parseArgs() -> URL? {
    let args = CommandLine.arguments
    guard let idx = args.firstIndex(of: "--text-file"), idx + 1 < args.count else {
        fputs("Usage: overlay_nonactivating --text-file <path>\n", stderr)
        return nil
    }
    return URL(fileURLWithPath: args[idx + 1])
}

guard let textURL = parseArgs() else {
    exit(2)
}

let app = NSApplication.shared
let delegate = OverlayApp(textFileURL: textURL)
app.delegate = delegate
app.run()
