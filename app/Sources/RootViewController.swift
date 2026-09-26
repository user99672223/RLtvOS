import UIKit

/// Hosts the Metal console view and refreshes it twice a second.
final class RootViewController: UIViewController {
    private let consoleView = ConsoleView()
    private var renderer: ConsoleRenderer?
    private var timer: Timer?

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        consoleView.frame = view.bounds
        consoleView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(consoleView)

        if let r = ConsoleRenderer() {
            r.attach(to: consoleView.metalLayer)
            renderer = r
            HostState.shared.renderer = r
            rl_log_str("host: metal device \(r.device.name) screen \(Int(UIScreen.main.bounds.width))x\(Int(UIScreen.main.bounds.height))@\(UIScreen.main.scale)")
        } else {
            rl_log_str("host: Metal unavailable")
        }
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.tick() }
        tick()
    }

    private func tick() {
        renderer?.render(lines: HostState.shared.consoleLines())
    }
}
