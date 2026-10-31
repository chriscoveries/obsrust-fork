#include "plugins/mac-videotoolbox/build/mac-videotoolbox.pch.h"

static _Atomic uint64_t total_bytes_copied = 0;
static _Atomic uint64_t total_submissions = 0;
static _Atomic uint64_t used_submission_slots = 0;
static _Atomic uint64_t submission_limit = UINT64_MAX;
static _Atomic bool force_raw_for_test = false;
static _Thread_local uint64_t pending_submission_copy_bytes;
static _Thread_local bool submission_in_progress;

#define VT_TIMING_CAPACITY 16384

struct vt_cpu_mark {
	uint64_t wall_ns;
	uint64_t user_ns;
	uint64_t system_ns;
	uint64_t service_ns;
	bool service_read;
};

static uint64_t timing_entry_to_output_ns[VT_TIMING_CAPACITY];
static uint64_t timing_submit_to_output_ns[VT_TIMING_CAPACITY];
static _Atomic uint64_t timing_samples = 0;
static _Atomic uint64_t timing_window_start = 0;
static struct vt_cpu_mark cpu_window_marks[2];
static _Atomic bool cpu_window_marked[2];
static _Atomic int watched_service_pid = 0;

void vt_copy_bytes_reset(void)
{
	atomic_store(&total_bytes_copied, 0);
	pending_submission_copy_bytes = 0;
	submission_in_progress = false;
}

uint64_t vt_copy_bytes_total(void)
{
	return atomic_load(&total_bytes_copied);
}

void vt_encode_submission_reset(uint64_t limit)
{
	atomic_store(&total_submissions, 0);
	atomic_store(&used_submission_slots, 0);
	atomic_store(&submission_limit, limit);
	pending_submission_copy_bytes = 0;
	submission_in_progress = false;
}

uint64_t vt_encode_submission_total(void)
{
	return atomic_load(&total_submissions);
}

bool vt_encode_submission_reserve(void)
{
	uint64_t used = atomic_load(&used_submission_slots);
	for (;;) {
		uint64_t limit = atomic_load(&submission_limit);
		if (used >= limit)
			return false;
		if (atomic_compare_exchange_weak(&used_submission_slots, &used, used + 1)) {
			pending_submission_copy_bytes = 0;
			submission_in_progress = true;
			return true;
		}
	}
}

static uint64_t vt_timeval_ns(struct timeval value)
{
	return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_usec * 1000ULL;
}

static void vt_cpu_mark_take(int index)
{
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return;
	cpu_window_marks[index].wall_ns = os_gettime_ns();
	cpu_window_marks[index].user_ns = vt_timeval_ns(usage.ru_utime);
	cpu_window_marks[index].system_ns = vt_timeval_ns(usage.ru_stime);
	cpu_window_marks[index].service_read = false;
	int pid = atomic_load(&watched_service_pid);
	struct rusage_info_v2 service;
	if (pid > 0 && proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&service) == 0) {
		/* rusage_info times are Mach absolute time units, not nanoseconds. */
		mach_timebase_info_data_t timebase;
		mach_timebase_info(&timebase);
		uint64_t ticks = service.ri_user_time + service.ri_system_time;
		cpu_window_marks[index].service_ns = ticks / timebase.denom * timebase.numer +
						     ticks % timebase.denom * timebase.numer / timebase.denom;
		cpu_window_marks[index].service_read = true;
	}
	atomic_store(&cpu_window_marked[index], true);
}

void vt_encode_submission_commit(void)
{
	if (!submission_in_progress)
		return;
	if (pending_submission_copy_bytes)
		atomic_fetch_add(&total_bytes_copied, pending_submission_copy_bytes);
	pending_submission_copy_bytes = 0;
	submission_in_progress = false;
	uint64_t accepted = atomic_fetch_add(&total_submissions, 1) + 1;
	if (accepted == atomic_load(&timing_window_start))
		vt_cpu_mark_take(0);
	if (accepted == atomic_load(&submission_limit))
		vt_cpu_mark_take(1);
}

void vt_encode_submission_cancel(void)
{
	if (!submission_in_progress)
		return;
	pending_submission_copy_bytes = 0;
	submission_in_progress = false;
	atomic_fetch_sub(&used_submission_slots, 1);
}

void vt_encode_timing_watch_service(int pid)
{
	atomic_store(&watched_service_pid, pid);
}

bool vt_encode_service_cpu_window(uint64_t *cpu_ns)
{
	if (!atomic_load(&cpu_window_marked[0]) || !atomic_load(&cpu_window_marked[1]) ||
	    !cpu_window_marks[0].service_read || !cpu_window_marks[1].service_read)
		return false;
	*cpu_ns = cpu_window_marks[1].service_ns - cpu_window_marks[0].service_ns;
	return true;
}

void vt_encode_timing_reset(uint64_t window_start)
{
	atomic_store(&timing_samples, 0);
	atomic_store(&timing_window_start, window_start);
	atomic_store(&cpu_window_marked[0], false);
	atomic_store(&cpu_window_marked[1], false);
}

uint64_t vt_encode_timing_now(void)
{
	return os_gettime_ns();
}

void vt_encode_timing_record(uint64_t entered_ns, uint64_t submitted_ns)
{
	uint64_t now = os_gettime_ns();
	uint64_t index = atomic_fetch_add(&timing_samples, 1);
	if (index >= VT_TIMING_CAPACITY)
		return;
	timing_entry_to_output_ns[index] = now - entered_ns;
	timing_submit_to_output_ns[index] = now - submitted_ns;
}

size_t vt_encode_timing_samples(uint64_t *entry_to_output_ns, uint64_t *submit_to_output_ns, size_t capacity)
{
	uint64_t recorded = atomic_load(&timing_samples);
	size_t count = recorded < VT_TIMING_CAPACITY ? (size_t)recorded : VT_TIMING_CAPACITY;
	if (count > capacity)
		count = capacity;
	memcpy(entry_to_output_ns, timing_entry_to_output_ns, count * sizeof(*entry_to_output_ns));
	memcpy(submit_to_output_ns, timing_submit_to_output_ns, count * sizeof(*submit_to_output_ns));
	return count;
}

bool vt_encode_cpu_window(uint64_t *frames, uint64_t *wall_ns, uint64_t *user_ns, uint64_t *system_ns)
{
	if (!atomic_load(&cpu_window_marked[0]) || !atomic_load(&cpu_window_marked[1]))
		return false;
	*frames = atomic_load(&submission_limit) - atomic_load(&timing_window_start);
	*wall_ns = cpu_window_marks[1].wall_ns - cpu_window_marks[0].wall_ns;
	*user_ns = cpu_window_marks[1].user_ns - cpu_window_marks[0].user_ns;
	*system_ns = cpu_window_marks[1].system_ns - cpu_window_marks[0].system_ns;
	return true;
}

void vt_test_force_raw(bool force_raw)
{
	atomic_store(&force_raw_for_test, force_raw);
}

bool vt_test_raw_forced(void)
{
	return atomic_load(&force_raw_for_test);
}

size_t vt_copy_frame_to_pixbuf(CVPixelBufferRef pixbuf, uint8_t *data[MAX_AV_PLANES], uint32_t linesize[MAX_AV_PLANES])
{
	size_t copied = 0;

	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (data[i] == NULL)
			break;
		uint8_t *p = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixbuf, i);
		uint8_t *f = data[i];
		size_t plane_linesize = CVPixelBufferGetBytesPerRowOfPlane(pixbuf, i);
		size_t plane_height = CVPixelBufferGetHeightOfPlane(pixbuf, i);

		for (size_t j = 0; j < plane_height; j++) {
			memcpy(p, f, linesize[i]);
			p += plane_linesize;
			f += linesize[i];
		}
		copied += linesize[i] * plane_height;
	}

	if (submission_in_progress)
		pending_submission_copy_bytes += copied;
	else
		atomic_fetch_add(&total_bytes_copied, copied);
	return copied;
}

/* File-local macros (BUILD.md): out of the next file in the unity group. */
#undef VT_TIMING_CAPACITY
