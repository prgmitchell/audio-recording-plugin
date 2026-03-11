#include "audio-recording-filter.h"
#include "plugin-support.h"

#include <math.h>
#include <string.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

enum trigger_mode {
	TRIGGER_MODE_FILTER_ACTIVE = 1,
	TRIGGER_MODE_OBS_STREAM_ACTIVE = 2,
	TRIGGER_MODE_OBS_RECORDING_ACTIVE = 3,
};

#define S_TRIGGER_MODE "trigger_mode"
#define S_OUTPUT_PATH "output_path"
#define S_FILENAME_PATTERN "filename_pattern"
#define S_GAIN_DB "gain_db"

struct audio_recording_filter;

struct writer_ops {
	const char *(*ext)(void);
	bool (*open)(struct audio_recording_filter *f, size_t channels);
	bool (*write)(struct audio_recording_filter *f, const int16_t *data, size_t samples);
	void (*close)(struct audio_recording_filter *f);
	const char *(*path)(const struct audio_recording_filter *f);
};

struct audio_recording_filter {
	obs_source_t *source;
	signal_handler_t *source_signals;
	pthread_mutex_t mutex;
	FILE *writer_file;
	char *writer_path;
	char *last_file;
	char *last_error;
	int16_t *pcm_interleaved;
	size_t pcm_capacity_samples;
	size_t writer_channels;
	uint32_t writer_sample_rate;
	uint64_t recording_start_ns;
	uint64_t frames_written;
	float gain_db;
	float gain_mul;
	bool recording;
	bool recording_requested;
	bool callback_registered;
	bool source_enable_cb_registered;
	bool status_dirty;
	char *status_text_cached;
	enum trigger_mode mode;
	char *output_path;
	char *filename_pattern;
	const struct writer_ops *writer;
};

static void refresh_properties_task(void *param);
static void update_status_cache_locked(struct audio_recording_filter *f);
static void maybe_queue_status_refresh(struct audio_recording_filter *f);
static void queue_source_update_task(obs_source_t *source);
static bool trigger_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings);
static void set_obs_mode_hint_visibility(obs_properties_t *props, obs_data_t *settings);
static void recompute_state_and_queue_refresh(struct audio_recording_filter *f, bool force_new_session);

static void set_last_error(struct audio_recording_filter *f, const char *msg)
{
	bfree(f->last_error);
	f->last_error = bstrdup(msg ? msg : "");
}

static void clear_last_error(struct audio_recording_filter *f)
{
	set_last_error(f, "");
}

static const char *current_status_text_locked(const struct audio_recording_filter *f)
{
	if (f->last_error && *f->last_error)
		return f->last_error;
	if (f->recording || f->recording_requested)
		return obs_module_text("StatusActive");
	return obs_module_text("StatusIdle");
}

static void update_status_cache_locked(struct audio_recording_filter *f)
{
	const char *next = current_status_text_locked(f);
	if (f->status_text_cached && strcmp(f->status_text_cached, next) == 0)
		return;

	bfree(f->status_text_cached);
	f->status_text_cached = bstrdup(next ? next : "");
	f->status_dirty = true;
}

static void maybe_queue_status_refresh(struct audio_recording_filter *f)
{
	obs_source_t *source = NULL;

	pthread_mutex_lock(&f->mutex);
	if (f->status_dirty) {
		f->status_dirty = false;
		source = obs_source_get_ref(f->source);
	}
	pthread_mutex_unlock(&f->mutex);

	if (source)
		obs_queue_task(OBS_TASK_UI, refresh_properties_task, source, false);
}

static void refresh_properties_task(void *param)
{
	obs_source_t *source = param;

	if (!source)
		return;

	obs_source_update_properties(source);
	obs_source_release(source);
}

static void source_update_task(void *param)
{
	obs_source_t *source = param;
	if (!source)
		return;
	obs_source_update(source, NULL);
	obs_source_release(source);
}

static void queue_source_update_task(obs_source_t *source)
{
	obs_source_t *source_ref = obs_source_get_ref(source);
	if (!source_ref)
		return;
	obs_queue_task(OBS_TASK_UI, source_update_task, source_ref, false);
}

static void set_obs_mode_hint_visibility(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *hint_prop = obs_properties_get(props, "obs_mode_hint");
	const long long mode = settings ? obs_data_get_int(settings, S_TRIGGER_MODE) : TRIGGER_MODE_FILTER_ACTIVE;
	const bool show = mode == TRIGGER_MODE_OBS_STREAM_ACTIVE || mode == TRIGGER_MODE_OBS_RECORDING_ACTIVE;

	if (hint_prop)
		obs_property_set_visible(hint_prop, show);
}

static bool trigger_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	set_obs_mode_hint_visibility(props, settings);
	/* Force immediate redraw/rebuild so mode-specific hint updates without reopening properties. */
	return true;
}

static void wav_write_le16(FILE *fp, uint16_t value)
{
	uint8_t b[2] = {(uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};
	fwrite(b, 1, sizeof(b), fp);
}

static void wav_write_le32(FILE *fp, uint32_t value)
{
	uint8_t b[4] = {(uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF), (uint8_t)((value >> 16) & 0xFF),
			(uint8_t)((value >> 24) & 0xFF)};
	fwrite(b, 1, sizeof(b), fp);
}

static bool write_wav_header(FILE *fp, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample)
{
	const uint16_t format_pcm = 1;
	const uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
	const uint16_t block_align = channels * (bits_per_sample / 8);

	if (fwrite("RIFF", 1, 4, fp) != 4)
		return false;
	wav_write_le32(fp, 0);
	if (fwrite("WAVE", 1, 4, fp) != 4)
		return false;
	if (fwrite("fmt ", 1, 4, fp) != 4)
		return false;
	wav_write_le32(fp, 16);
	wav_write_le16(fp, format_pcm);
	wav_write_le16(fp, channels);
	wav_write_le32(fp, sample_rate);
	wav_write_le32(fp, byte_rate);
	wav_write_le16(fp, block_align);
	wav_write_le16(fp, bits_per_sample);
	if (fwrite("data", 1, 4, fp) != 4)
		return false;
	wav_write_le32(fp, 0);
	return !ferror(fp);
}

static bool finalize_wav_file(FILE *fp)
{
	long end = ftell(fp);
	if (end < 44)
		return false;
	if (end > 0xFFFFFFFFL)
		return false;

	const uint32_t file_size = (uint32_t)end;
	const uint32_t data_size = file_size - 44;
	const uint32_t riff_size = file_size - 8;

	if (fseek(fp, 4, SEEK_SET) != 0)
		return false;
	wav_write_le32(fp, riff_size);
	if (fseek(fp, 40, SEEK_SET) != 0)
		return false;
	wav_write_le32(fp, data_size);
	return fflush(fp) == 0;
}

static const char *filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioRecordingFilter");
}

static void build_output_filename(struct audio_recording_filter *f, struct dstr *path_out)
{
	struct dstr file_name = {0};
	struct dstr base_name = {0};
	struct dstr full = {0};
	char *generated = NULL;
	int suffix = 0;

	dstr_init(path_out);
	if (!f->output_path || !*f->output_path || !f->filename_pattern || !*f->filename_pattern)
		return;

	while (true) {
		bfree(generated);
		generated = os_generate_formatted_filename(f->writer->ext(), true, f->filename_pattern);
		if (!generated)
			break;

		dstr_copy(&file_name, generated);
		if (suffix > 0) {
			/* Remove extension and append numbered suffix */
			const size_t len = file_name.len;
			const char *ext = f->writer->ext();
			struct dstr ext_with_dot = {0};
			dstr_printf(&ext_with_dot, ".%s", ext);
			if (len > ext_with_dot.len &&
			    strcmp(file_name.array + len - ext_with_dot.len, ext_with_dot.array) == 0)
				file_name.array[len - ext_with_dot.len] = '\0';
			dstr_copy(&base_name, file_name.array);
			dstr_printf(&file_name, "%s_%d.%s", base_name.array, suffix, ext);
			dstr_free(&ext_with_dot);
		}

		dstr_printf(&full, "%s/%s", f->output_path, file_name.array);
		if (!os_file_exists(full.array))
			break;

		suffix++;
		dstr_free(&full);
		dstr_init(&full);
	}

	if (full.len)
		dstr_copy(path_out, full.array);

	dstr_free(&full);
	dstr_free(&file_name);
	dstr_free(&base_name);
	bfree(generated);
}

static const char *wav_ext(void)
{
	return "wav";
}

static bool wav_open_writer_locked(struct audio_recording_filter *f, size_t channels)
{
	audio_t *audio = obs_get_audio();
	const uint32_t sample_rate = audio_output_get_sample_rate(audio);
	const size_t fallback_channels = audio_output_get_channels(audio);
	const size_t actual_channels = channels ? channels : fallback_channels;
	struct dstr path = {0};
	FILE *fp = NULL;

	if (!f->output_path || !*f->output_path) {
		set_last_error(f, "Output path is empty");
		return false;
	}

	if (os_mkdirs(f->output_path) == MKDIR_ERROR) {
		set_last_error(f, "Failed to create output directory");
		return false;
	}

	build_output_filename(f, &path);
	if (!path.len) {
		set_last_error(f, "Failed to generate output filename");
		dstr_free(&path);
		return false;
	}

	fp = os_fopen(path.array, "wb");
	if (!fp) {
		set_last_error(f, "Failed to open WAV file for writing");
		dstr_free(&path);
		return false;
	}

	if (!write_wav_header(fp, sample_rate, (uint16_t)actual_channels, 16)) {
		fclose(fp);
		set_last_error(f, "Failed to write WAV header");
		dstr_free(&path);
		return false;
	}

	if (f->writer_file)
		fclose(f->writer_file);
	f->writer_file = fp;
	f->writer_channels = actual_channels;
	f->writer_sample_rate = sample_rate;
	f->recording_start_ns = os_gettime_ns();
	f->frames_written = 0;
	bfree(f->writer_path);
	f->writer_path = bstrdup(path.array);
	dstr_free(&path);
	return true;
}

static bool wav_write_writer_locked(struct audio_recording_filter *f, const int16_t *data, size_t samples)
{
	return f->writer_file && fwrite(data, sizeof(int16_t), samples, f->writer_file) == samples;
}

static void wav_close_writer_locked(struct audio_recording_filter *f)
{
	if (!f->writer_file)
		return;

	if (!finalize_wav_file(f->writer_file) && (!f->last_error || !*f->last_error))
		set_last_error(f, "Failed to finalize WAV header");
	fclose(f->writer_file);
	f->writer_file = NULL;
}

static const char *wav_path_locked(const struct audio_recording_filter *f)
{
	return f->writer_path;
}

static const struct writer_ops wav_writer_ops = {
	.ext = wav_ext,
	.open = wav_open_writer_locked,
	.write = wav_write_writer_locked,
	.close = wav_close_writer_locked,
	.path = wav_path_locked,
};

static uint64_t ns_to_frames(uint64_t ns, uint32_t sample_rate)
{
	if (sample_rate == 0)
		return 0;
	return (ns * sample_rate) / 1000000000ULL;
}

static void ensure_pcm_capacity_locked(struct audio_recording_filter *f, size_t samples_needed)
{
	if (samples_needed > f->pcm_capacity_samples) {
		f->pcm_interleaved = brealloc(f->pcm_interleaved, samples_needed * sizeof(int16_t));
		f->pcm_capacity_samples = samples_needed;
	}
}

static bool append_silence_frames_locked(struct audio_recording_filter *f, uint64_t frames)
{
	while (frames > 0) {
		const uint64_t chunk_frames = frames > 4096 ? 4096 : frames;
		const size_t samples = (size_t)chunk_frames * f->writer_channels;
		ensure_pcm_capacity_locked(f, samples);
		memset(f->pcm_interleaved, 0, samples * sizeof(int16_t));
		if (!f->writer->write(f, f->pcm_interleaved, samples))
			return false;
		f->frames_written += chunk_frames;
		frames -= chunk_frames;
	}
	return true;
}

static bool pad_silence_to_now_locked(struct audio_recording_filter *f, uint64_t next_frames)
{
	const uint64_t now_ns = os_gettime_ns();
	const uint64_t elapsed_frames = ns_to_frames(now_ns - f->recording_start_ns, f->writer_sample_rate);
	const uint64_t expected_with_next = f->frames_written + next_frames;

	if (elapsed_frames <= expected_with_next)
		return true;

	return append_silence_frames_locked(f, elapsed_frames - expected_with_next);
}

static size_t detect_audio_channels(const struct obs_audio_data *audio)
{
	size_t channels = 0;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		if (audio->data[i])
			channels = i + 1;
	}
	return channels;
}

static bool start_recording_locked(struct audio_recording_filter *f, size_t channels)
{
	if (f->recording)
		return true;

	if (!f->writer->open(f, channels))
		return false;
	f->recording = true;
	clear_last_error(f);
	obs_log(LOG_INFO, "started recording: %s", f->writer->path(f));
	return true;
}

static void stop_recording_locked(struct audio_recording_filter *f)
{
	const char *path = f->writer->path(f);

	if (!f->recording)
		return;

	if (path && *path) {
		bfree(f->last_file);
		f->last_file = bstrdup(path);
	}
	if (!pad_silence_to_now_locked(f, 0) && (!f->last_error || !*f->last_error))
		set_last_error(f, "Write error while padding silence");
	f->writer->close(f);
	f->recording = false;
	f->writer_sample_rate = 0;
	f->recording_start_ns = 0;
	f->frames_written = 0;
	if (f->last_file && *f->last_file)
		obs_log(LOG_INFO, "stopped recording: %s", f->last_file);
}

static bool desired_recording_locked(struct audio_recording_filter *f)
{
	const bool source_enabled = obs_source_enabled(f->source);
	const bool obs_streaming_now = obs_frontend_streaming_active();
	const bool obs_recording_now = obs_frontend_recording_active();

	if (f->mode == TRIGGER_MODE_FILTER_ACTIVE) {
		return source_enabled;
	}
	if (f->mode == TRIGGER_MODE_OBS_STREAM_ACTIVE) {
		return obs_streaming_now && source_enabled;
	}
	if (f->mode == TRIGGER_MODE_OBS_RECORDING_ACTIVE) {
		return obs_recording_now && source_enabled;
	}
	return false;
}

static void apply_recording_state_locked(struct audio_recording_filter *f, size_t channels_hint)
{
	const bool should_record = desired_recording_locked(f);

	f->recording_requested = should_record;

	if (!should_record) {
		stop_recording_locked(f);
	} else if (!f->recording) {
		start_recording_locked(f, channels_hint);
	}

	update_status_cache_locked(f);
}

static void recompute_state_and_queue_refresh(struct audio_recording_filter *f, bool force_new_session)
{
	pthread_mutex_lock(&f->mutex);
	if (force_new_session)
		stop_recording_locked(f);
	apply_recording_state_locked(f, 0);
	pthread_mutex_unlock(&f->mutex);
	maybe_queue_status_refresh(f);
}

static int16_t float_to_s16(float v)
{
	if (v > 1.0f)
		v = 1.0f;
	else if (v < -1.0f)
		v = -1.0f;

	if (v >= 0.0f)
		return (int16_t)lrintf(v * 32767.0f);
	return (int16_t)lrintf(v * 32768.0f);
}

static float db_to_mul(float db)
{
	return powf(10.0f, db / 20.0f);
}

static struct obs_audio_data *audio_filter(void *data, struct obs_audio_data *audio)
{
	struct audio_recording_filter *f = data;
	size_t samples_needed = 0;
	size_t idx = 0;
	const size_t detected_channels = detect_audio_channels(audio);

	pthread_mutex_lock(&f->mutex);

	apply_recording_state_locked(f, detected_channels);

	if (!f->recording || !f->writer_file) {
		pthread_mutex_unlock(&f->mutex);
		maybe_queue_status_refresh(f);
		return audio;
	}

	if (!pad_silence_to_now_locked(f, audio->frames)) {
		set_last_error(f, "Write error while padding silence");
		stop_recording_locked(f);
		update_status_cache_locked(f);
		pthread_mutex_unlock(&f->mutex);
		maybe_queue_status_refresh(f);
		return audio;
	}

	samples_needed = (size_t)audio->frames * f->writer_channels;
	ensure_pcm_capacity_locked(f, samples_needed);

	for (uint32_t frame = 0; frame < audio->frames; frame++) {
		for (size_t ch = 0; ch < f->writer_channels; ch++) {
			const float *in = (const float *)audio->data[ch];
			const float v = in ? in[frame] * f->gain_mul : 0.0f;
			f->pcm_interleaved[idx++] = float_to_s16(v);
		}
	}

	if (!f->writer->write(f, f->pcm_interleaved, idx)) {
		set_last_error(f, "Write error while recording WAV");
		stop_recording_locked(f);
		update_status_cache_locked(f);
	} else {
		f->frames_written += audio->frames;
	}

	pthread_mutex_unlock(&f->mutex);
	maybe_queue_status_refresh(f);
	return audio;
}

static void free_filter(struct audio_recording_filter *f)
{
	if (!f)
		return;

	pthread_mutex_destroy(&f->mutex);
	bfree(f->writer_path);
	bfree(f->last_file);
	bfree(f->last_error);
	bfree(f->status_text_cached);
	bfree(f->output_path);
	bfree(f->filename_pattern);
	bfree(f->pcm_interleaved);
	bfree(f);
}

static void frontend_event(enum obs_frontend_event event, void *private_data)
{
	struct audio_recording_filter *f = private_data;
	bool should_update = false;
	if (!f)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		should_update = true;
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		should_update = true;
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		should_update = true;
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		should_update = true;
		break;
	default:
		break;
	}
	if (should_update)
		queue_source_update_task(f->source);
}

static void source_enable_event(void *private_data, calldata_t *cd)
{
	struct audio_recording_filter *f = private_data;
	const bool enabled = calldata_bool(cd, "enabled");
	bool force_new_session = false;

	if (!f)
		return;

	pthread_mutex_lock(&f->mutex);
	force_new_session = f->mode == TRIGGER_MODE_FILTER_ACTIVE && enabled;
	pthread_mutex_unlock(&f->mutex);

	recompute_state_and_queue_refresh(f, force_new_session);
}

static void update_from_settings_locked(struct audio_recording_filter *f, obs_data_t *settings)
{
	long long configured_mode = obs_data_get_int(settings, S_TRIGGER_MODE);
	enum trigger_mode new_mode = (enum trigger_mode)configured_mode;
	const char *new_path = obs_data_get_string(settings, S_OUTPUT_PATH);
	const char *new_pattern = obs_data_get_string(settings, S_FILENAME_PATTERN);
	const double new_gain = obs_data_get_double(settings, S_GAIN_DB);

	/* Migrate legacy manual mode to filter active. */
	if (configured_mode == 0)
		new_mode = TRIGGER_MODE_FILTER_ACTIVE;

	bfree(f->output_path);
	f->output_path = bstrdup(new_path ? new_path : "");
	bfree(f->filename_pattern);
	f->filename_pattern = bstrdup(new_pattern ? new_pattern : "");
	f->gain_db = (float)new_gain;
	f->gain_mul = db_to_mul(f->gain_db);
	f->mode = new_mode;
	/* All modes arm via common state engine; writer opens on first audio callback. */
	apply_recording_state_locked(f, 0);
}

static void update_filter(void *data, obs_data_t *settings)
{
	struct audio_recording_filter *f = data;
	pthread_mutex_lock(&f->mutex);
	update_from_settings_locked(f, settings);
	pthread_mutex_unlock(&f->mutex);
	maybe_queue_status_refresh(f);
}

static void *create_filter(obs_data_t *settings, obs_source_t *source)
{
	struct audio_recording_filter *f = bzalloc(sizeof(*f));
	pthread_mutex_init_value(&f->mutex);
	f->source = source;
	f->source_signals = obs_source_get_signal_handler(source);
	f->output_path = bstrdup("");
	f->filename_pattern = bstrdup("%CCYY-%MM-%DD_%hh-%mm-%ss");
	f->last_error = bstrdup("");
	f->recording_requested = false;
	f->gain_db = 0.0f;
	f->gain_mul = 1.0f;
	f->status_text_cached = bstrdup(obs_module_text("StatusIdle"));
	f->writer = &wav_writer_ops;
	obs_frontend_add_event_callback(frontend_event, f);
	f->callback_registered = true;
	if (f->source_signals) {
		signal_handler_connect(f->source_signals, "enable", source_enable_event, f);
		f->source_enable_cb_registered = true;
	}
	update_filter(f, settings);
	return f;
}

static void destroy_filter(void *data)
{
	struct audio_recording_filter *f = data;
	if (!f)
		return;

	if (f->callback_registered) {
		obs_frontend_remove_event_callback(frontend_event, f);
		f->callback_registered = false;
	}
	if (f->source_enable_cb_registered && f->source_signals) {
		signal_handler_disconnect(f->source_signals, "enable", source_enable_event, f);
		f->source_enable_cb_registered = false;
	}

	pthread_mutex_lock(&f->mutex);
	stop_recording_locked(f);
	pthread_mutex_unlock(&f->mutex);
	free_filter(f);
}

static void activate_filter(void *data)
{
	struct audio_recording_filter *f = data;
	recompute_state_and_queue_refresh(f, false);
}

static void deactivate_filter(void *data)
{
	struct audio_recording_filter *f = data;
	recompute_state_and_queue_refresh(f, false);
}

static void build_status_text_locked(struct audio_recording_filter *f, struct dstr *status, bool *has_error_out)
{
	const bool has_error = f->last_error && *f->last_error;

	*has_error_out = has_error;

	if (has_error) {
		dstr_printf(status, "%s: %s", obs_module_text("StatusError"), f->last_error);
		return;
	}

	dstr_copy(status, f->status_text_cached ? f->status_text_cached : obs_module_text("StatusIdle"));
}

static obs_properties_t *filter_properties(void *data)
{
	struct audio_recording_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode_prop = NULL;
	obs_property_t *status_prop = NULL;
	obs_property_t *hint_prop = NULL;
	obs_data_t *current_settings = NULL;
	struct dstr status = {0};
	bool has_error = false;
	bool is_active = false;

	status_prop = obs_properties_add_text(props, "status_text", obs_module_text("StatusLabel"), OBS_TEXT_INFO);

	mode_prop = obs_properties_add_list(props, S_TRIGGER_MODE, obs_module_text("TriggerMode"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode_prop, obs_module_text("TriggerMode.FilterActive"), TRIGGER_MODE_FILTER_ACTIVE);
	obs_property_list_add_int(mode_prop, obs_module_text("TriggerMode.ObsStreamActive"),
				  TRIGGER_MODE_OBS_STREAM_ACTIVE);
	obs_property_list_add_int(mode_prop, obs_module_text("TriggerMode.ObsRecordingActive"),
				  TRIGGER_MODE_OBS_RECORDING_ACTIVE);
	obs_property_set_modified_callback(mode_prop, trigger_mode_modified);

	obs_properties_add_path(props, S_OUTPUT_PATH, obs_module_text("OutputPath"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, S_FILENAME_PATTERN, obs_module_text("FilenamePattern"), OBS_TEXT_DEFAULT);
	obs_properties_add_float_slider(props, S_GAIN_DB, obs_module_text("GainDb"), -60.0, 24.0, 0.1);
	hint_prop =
		obs_properties_add_text(props, "obs_mode_hint", obs_module_text("ObsModeEnabledHint"), OBS_TEXT_INFO);

	if (f) {
		pthread_mutex_lock(&f->mutex);
		build_status_text_locked(f, &status, &has_error);
		is_active = f->recording;
		pthread_mutex_unlock(&f->mutex);
		current_settings = obs_source_get_settings(f->source);
	} else {
		dstr_copy(&status, obs_module_text("StatusIdle"));
	}

	{
		struct dstr status_line = {0};
		dstr_printf(&status_line, "%s: %s", obs_module_text("StatusLabel"),
			    status.array ? status.array : obs_module_text("StatusIdle"));
		obs_property_set_description(status_prop,
					     status_line.array ? status_line.array : obs_module_text("StatusIdle"));
		dstr_free(&status_line);
	}
	obs_property_text_set_info_type(status_prop, has_error ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL);
	obs_property_set_enabled(mode_prop, !is_active);
	dstr_free(&status);

	if (hint_prop)
		set_obs_mode_hint_visibility(props, current_settings);

	if (current_settings)
		obs_data_release(current_settings);

	return props;
}

static void filter_defaults(obs_data_t *settings)
{
	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : NULL;
	const bool adv_out = mode && (strcmp(mode, "Advanced") == 0 || strcmp(mode, "advanced") == 0);
	const char *default_path = NULL;
	const char *default_filename = NULL;
	bool free_default_path = false;

	if (config) {
		if (adv_out)
			default_path = config_get_string(config, "AdvOut", "RecFilePath");
		else
			default_path = config_get_string(config, "SimpleOutput", "FilePath");
		default_filename = config_get_string(config, "Output", "FilenameFormatting");
	}

	if (!default_path || !*default_path) {
		default_path = os_get_abs_path_ptr(".");
		free_default_path = true;
	}
	if (!default_path)
		default_path = ".";
	if (!default_filename || !*default_filename)
		default_filename = "%CCYY-%MM-%DD_%hh-%mm-%ss";

	obs_data_set_default_int(settings, S_TRIGGER_MODE, TRIGGER_MODE_OBS_RECORDING_ACTIVE);
	obs_data_set_default_string(settings, S_OUTPUT_PATH, default_path);
	obs_data_set_default_string(settings, S_FILENAME_PATTERN, default_filename);
	obs_data_set_default_double(settings, S_GAIN_DB, 0.0);

	if (free_default_path)
		bfree((char *)default_path);
}

struct obs_source_info audio_recording_filter_info = {
	.id = "audio_recording_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = filter_name,
	.create = create_filter,
	.destroy = destroy_filter,
	.update = update_filter,
	.filter_audio = audio_filter,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.activate = activate_filter,
	.deactivate = deactivate_filter,
};
