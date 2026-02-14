import AppKit
import Foundation

final class OverlayApp: NSObject, NSApplicationDelegate {
    private let textFileURL: URL
    private let sessionDirURL: URL
    private let panel = NSPanel(
        contentRect: NSRect(x: 0, y: 0, width: 620, height: 240),
        styleMask: [.borderless, .nonactivatingPanel],
        backing: .buffered,
        defer: false
    )
    private let textField = NSTextField(wrappingLabelWithString: "")
    private let metaField = NSTextField(wrappingLabelWithString: "")
    private let plusButton = NSButton(title: "+", target: nil, action: nil)
    private let minusButton = NSButton(title: "-", target: nil, action: nil)
    private var timer: Timer?
    private var eventMonitor: Any?
    private var lastText = ""

    init(textFileURL: URL, sessionDirURL: URL) {
        self.textFileURL = textFileURL
        self.sessionDirURL = sessionDirURL
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
        eventMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] event in
            guard let self else { return event }
            let p = event.locationInWindow
            let screenPoint = event.window?.convertPoint(toScreen: p) ?? p
            if self.panel.frame.contains(screenPoint) {
                self.requestStopPlayback()
            }
            return event
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

        plusButton.bezelStyle = .rounded
        plusButton.controlSize = .small
        plusButton.font = NSFont.monospacedSystemFont(ofSize: 13, weight: .bold)
        plusButton.target = self
        plusButton.action = #selector(votePlus)

        minusButton.bezelStyle = .rounded
        minusButton.controlSize = .small
        minusButton.font = NSFont.monospacedSystemFont(ofSize: 13, weight: .bold)
        minusButton.target = self
        minusButton.action = #selector(voteMinus)

        let container = NSView(frame: panel.contentView!.bounds)
        container.wantsLayer = true
        container.layer?.backgroundColor = NSColor.clear.cgColor
        panel.contentView = container
        container.addSubview(textField)
        container.addSubview(metaField)
        container.addSubview(plusButton)
        container.addSubview(minusButton)
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
        let buttonBottom: CGFloat = 8
        let buttonWidth: CGFloat = 30
        let buttonHeight: CGFloat = 24
        let buttonGap: CGFloat = 6
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
        let trimmedMain = mainText.trimmingCharacters(in: .whitespacesAndNewlines)
        let lowerMain = trimmedMain.lowercased()
        let showFeedbackButtons =
            !trimmedMain.isEmpty &&
            !lowerMain.hasPrefix("recording active.") &&
            !lowerMain.hasPrefix("waiting for chunk advice") &&
            !lowerMain.hasPrefix("thinking...")
        plusButton.isHidden = !showFeedbackButtons
        minusButton.isHidden = !showFeedbackButtons
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
        if showFeedbackButtons {
            let buttonY = buttonBottom
            plusButton.frame = NSRect(
                x: width - innerPadX - buttonWidth,
                y: buttonY,
                width: buttonWidth,
                height: buttonHeight
            )
            minusButton.frame = NSRect(
                x: width - innerPadX - (buttonWidth * 2) - buttonGap,
                y: buttonY,
                width: buttonWidth,
                height: buttonHeight
            )
        }
    }

    @objc private func votePlus() {
        requestStopPlayback()
        writeFeedback("+")
    }

    @objc private func voteMinus() {
        requestStopPlayback()
        writeFeedback("-")
    }

    private func requestStopPlayback() {
        let stopPath = sessionDirURL.appendingPathComponent("_stop_playback.flag")
        try? "stop\n".write(to: stopPath, atomically: true, encoding: .utf8)
    }

    private func writeFeedback(_ vote: String) {
        let currentPath = sessionDirURL.appendingPathComponent("_current_advice_chunk.txt")
        guard let currentRaw = try? String(contentsOf: currentPath, encoding: .utf8) else {
            NSSound.beep()
            return
        }
        let adviceStem = currentRaw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !adviceStem.isEmpty else {
            NSSound.beep()
            return
        }
        let feedbackPath = sessionDirURL.appendingPathComponent("\(adviceStem)_advice_feedback.txt")
        let existing = (try? String(contentsOf: feedbackPath, encoding: .utf8)) ?? ""
        let line = "\(Int(Date().timeIntervalSince1970))\t\(vote)\n"
        let updated = existing + line
        do {
            try updated.write(to: feedbackPath, atomically: true, encoding: .utf8)
        } catch {
            NSSound.beep()
        }
    }
}

private func parseArgs() -> (URL, URL)? {
    let args = CommandLine.arguments
    guard
        let textIdx = args.firstIndex(of: "--text-file"),
        textIdx + 1 < args.count,
        let sessionIdx = args.firstIndex(of: "--session-dir"),
        sessionIdx + 1 < args.count
    else {
        fputs("Usage: overlay_nonactivating --text-file <path> --session-dir <path>\n", stderr)
        return nil
    }
    let textURL = URL(fileURLWithPath: args[textIdx + 1])
    let sessionURL = URL(fileURLWithPath: args[sessionIdx + 1])
    return (textURL, sessionURL)
}

guard let (textURL, sessionURL) = parseArgs() else {
    exit(2)
}

let app = NSApplication.shared
let delegate = OverlayApp(textFileURL: textURL, sessionDirURL: sessionURL)
app.delegate = delegate
app.run()
