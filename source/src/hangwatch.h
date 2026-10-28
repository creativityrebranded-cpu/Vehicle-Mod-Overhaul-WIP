#pragma once

// Loading-hang diagnosis (0.56.1): when the planet manager stays in its loading state, the load thread, the last
// frame-task thread and every thread with one of our plugins on its stack are unwound into the log (15 s, 45 s and
// 120 s into the stall), so a hang names the code it waits in.
namespace hangwatch
{
	void BeginShift();
	void ShiftStage(const char* stage, unsigned cells, unsigned refs);
	void ShiftProgress(unsigned cells, unsigned refs);
	void ShiftRefs(unsigned refs);
	void EndShift();
	void NoteLoadThread();   // called from the planet manager setup hook (the load)
	void NoteFrame();        // called from the frame task
	void Check(bool managerLoading);  // diagnostics thread, every 0.5 s
}
