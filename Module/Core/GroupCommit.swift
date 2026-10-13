// The group-commit timer: commits the volume's journal when the interval elapsed since the last
// commit (bw_commit_due, bwntfs.h:81), and reports a fail-stop. Runs on EngineQueue.queue.
import Dispatch
import Foundation

final class GroupCommit: @unchecked Sendable {
    private let volume: EngineVolume
    private let intervalMilliseconds: Int
    private let onFailure: @Sendable (Int32) -> Void
    private var timer: (any DispatchSourceTimer)?
    private var reported = false

    init(volume: EngineVolume, intervalMilliseconds: Int, onFailure: @escaping @Sendable (Int32) -> Void) {
        self.volume = volume
        self.intervalMilliseconds = intervalMilliseconds
        self.onFailure = onFailure
    }

    /// Ticks four times per interval (at least every 250 ms) so a commit is at most ~25 % late.
    func start() {
        EngineQueue.assertOn()
        guard timer == nil, !volume.isReadOnly else { return }
        let t = DispatchSource.makeTimerSource(queue: EngineQueue.queue)
        let tick = max(250, intervalMilliseconds / 4)
        t.schedule(deadline: .now() + .milliseconds(tick), repeating: .milliseconds(tick), leeway: .milliseconds(tick / 4))
        t.setEventHandler { [weak self] in self?.tick() }
        timer = t
        t.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    /// One timer tick; public for the tests.
    func tick() {
        EngineQueue.assertOn()
        guard volume.isMounted else { stop(); return }
        if volume.failure() != 0 { report(volume.failure()); return }
        guard volume.commitDue() else { return }
        do { try volume.sync() } catch { report(error.code) }
    }

    private func report(_ code: Int32) {
        stop()
        guard !reported else { return }
        reported = true
        onFailure(code)
    }
}
