import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        HostState.shared.bootstrap()
        let win = UIWindow(frame: UIScreen.main.bounds)
        win.rootViewController = RootViewController()
        win.makeKeyAndVisible()
        window = win
        application.isIdleTimerDisabled = true
        DebugServer.shared.start(port: 7777)
        return true
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        application.isIdleTimerDisabled = true
        rl_log_str("host: did become active")
    }

    func applicationWillResignActive(_ application: UIApplication) {
        rl_log_str("host: will resign active")
    }

    func applicationDidEnterBackground(_ application: UIApplication) {
        rl_log_str("host: did enter background")
    }

    func applicationDidReceiveMemoryWarning(_ application: UIApplication) {
        rl_log_str("host: MEMORY WARNING \(HostState.compact(HostState.shared.memInfo()))")
    }
}
