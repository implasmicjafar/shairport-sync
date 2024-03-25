/*
 * libalsa output driver. This file is part of Shairport.
 * Copyright (c) Muffinman, Skaman 2013
 * Copyright (c) Mike Brady 2014 -- 2022
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define ALSA_PCM_NEW_HW_PARAMS_API

#include "audio_alsa.h"

typedef struct alsa_output_ alsa_output;

// Forward declaration of functions used in the audio_multi_alsa object.
static void help(void);
static int init(int argc, char **argv);
static void deinit(void);
static void start(int i_sample_rate, int i_sample_format);
static int play(void *buf, int samples, __attribute__((unused)) int sample_type,
                __attribute__((unused)) uint32_t timestamp,
                __attribute__((unused)) uint64_t playtime);
static void stop(void);
static void flush(void);
static int delay(long *the_delay);
static int stats(uint64_t *raw_measurement_time, uint64_t *corrected_measurement_time,
                 uint64_t *the_delay, uint64_t *frames_sent_to_dac);

static void *alsa_buffer_monitor_thread_code(void *arg);

static void volume(double vol);
static void do_volume(alsa_output *output, double vol);
static int prepare(void);
static int do_play(alsa_output *output, void *buf, int samples);

static void parameters(audio_parameters *info);
static int mute(int do_mute); // returns true if it actually is allowed to use the mute
static char *default_default = "default";

static int precision_delay_and_status(alsa_output *output, snd_pcm_state_t *state, snd_pcm_sframes_t *delay,
                                      yndk_type *using_update_timestamps);
static int standard_delay_and_status(alsa_output *output, snd_pcm_state_t *state, snd_pcm_sframes_t *delay,
                                     yndk_type *using_update_timestamps);

audio_output audio_multi_alsa = {
    .name = "multi_alsa",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .prepare = &prepare,
    .start = &start,
    .stop = &stop,
    .is_running = NULL,
    .flush = &flush,
    .delay = &delay,
    .play = &play,
    .stats = &stats,   // will also include frames of silence sent to stop
                       // standby mode
                       //    .rate_info = NULL,
    .mute = NULL,      // a function will be provided if it can, and is allowed to,
                       // do hardware mute
    .volume = NULL,    // a function will be provided if it can do hardware volume
    .parameters = NULL // a function will be provided if it can do hardware volume
};

extern void handle_unfixable_error(int errorCode);

// This array is a sequence of the output rates to be tried if automatic speed selection is
// requested.
// There is no benefit to upconverting the frame rate, other than for compatibility.
// The lowest rate that the DAC is capable of is chosen.

extern unsigned int auto_speed_output_rates[];

extern const int number_speeds;

// This array is the sequence of formats to be tried if automatic selection of the format is
// requested.
// Ideally, audio should pass through Shairport Sync unaltered, apart from occasional interpolation.
// If the user chooses a hardware mixer, then audio could go straight through, unaltered, as signed
// 16 bit stereo.
// However, the user might, at any point, select an option that requires modification, such as
// stereo to mono mixing,
// additional volume attenuation, convolution, and so on. For this reason,
// we look for the greatest depth the DAC is capable of, since upconverting it is completely
// lossless.
// If audio processing is required, then the dither that must be added will
// be added at the lowest possible level.
// Hence, selecting the greatest bit depth is always either beneficial or neutral.

extern sps_format_t auto_format_check_sequence[];

extern const int number_sps_format_t;

// This array is of all the formats known to Shairport Sync, in order of the SPS_FORMAT definitions,
// with their equivalent alsa codes and their frame sizes.
// If just one format is requested, then its entry is searched for in the array and checked on the
// device
// If auto format is requested, then each entry in turn is tried until a working format is found.
// So, it should be in the search order.

extern format_record fr[];

struct alsa_output_
{
    int output_method_signalled; // = 0; // for reporting whether it's using mmap or not
    int delay_type_notified;     // = -1; // for controlling the reporting of whether the output device can do
                                 // precision delays (e.g. alsa->pulsaudio virtual devices can't)
    int use_monotonic_clock;     // = 0;  // this value will be set when the hardware is initialised

    pthread_t alsa_buffer_monitor_thread;
    // for deciding when to activate mute
    // there are two sources of requests to mute -- the backend itself, e.g. when it
    // is flushing
    // and the player, e.g. when volume goes down to -144, i.e. mute.

    // we may not be allowed to use hardware mute, so we must reflect that too.
    int mute_requested_externally;
    int mute_requested_internally;

    // for tracking how long the output device has stalled
    uint64_t stall_monitor_start_time;      // zero if not initialised / not started /
                                            // zeroed by flush
    long stall_monitor_frame_count;         // set to delay at start of time, incremented by
                                            // any writes
    uint64_t stall_monitor_error_threshold; // if the time is longer than this, it's
                                            // an error

    snd_output_t *output; // = NULL;
    int frame_size;       // in bytes for interleaved stereo

    int alsa_device_initialised; // boolean to ensure the initialisation is only
                                 // done once

    yndk_type precision_delay_available_status; // = YNDK_DONT_KNOW; // initially, we don't know if the device can do precision delay

    snd_pcm_t *alsa_handle;             // = NULL;
    int alsa_handle_status;             // = ENODEV; // if alsa_handle is NULL, this should say why with a unix error code
    snd_pcm_hw_params_t *alsa_params;   // = NULL;
    snd_pcm_sw_params_t *alsa_swparams; // = NULL;
    snd_ctl_t *ctl;                     // = NULL;
    snd_ctl_elem_id_t *elem_id;         // = NULL;
    snd_mixer_t *alsa_mix_handle;       // = NULL;
    snd_mixer_elem_t *alsa_mix_elem;    // = NULL;
    snd_mixer_selem_id_t *alsa_mix_sid; // = NULL;
    long alsa_mix_minv, alsa_mix_maxv;
    long alsa_mix_mindb, alsa_mix_maxdb;

    char *alsa_out_dev;  // = "default";
    char *alsa_mix_dev;  // = NULL;
    char *alsa_mix_ctrl; // = NULL;
    int alsa_mix_index;  // = 0;
    int has_softvol;     // = 0;

    pthread_mutex_t alsa_mutex;
    pthread_mutex_t alsa_mixer_mutex;

    int64_t dither_random_number_store; // = 0;

    int volume_set_request; // = 0; // set when an external request is made to set the volume.

    int mixer_volume_setting_gives_mute; // = 0; // set when it is discovered that
                                         // particular mixer volume setting
                                         // causes a mute.
    long alsa_mix_mute;                  // setting the volume to this value mutes output, if
                                         // mixer_volume_setting_gives_mute is true
    int volume_based_mute_is_active;     // = 0; // set when muting is being done by a setting the volume to a magic value

    // use this to allow the use of snd_pcm_writei or snd_pcm_mmap_writei
    snd_pcm_sframes_t (*alsa_pcm_write)(snd_pcm_t *, const void *, snd_pcm_uframes_t); // = snd_pcm_writei;
    int set_period_size_request;
    int set_buffer_size_request;
    snd_pcm_uframes_t period_size_requested, buffer_size_requested;
    /*
    int no_sync;
    int alsa_use_hardware_mute;
    int output_format_auto_requested;
    int output_rate_auto_requested;
    unsigned int output_rate;
    int no_mmap;
    double disable_standby_mode_silence_threshold;
    double disable_standby_mode_silence_scan_interval;
    disable_standby_mode_type disable_standby_mode;
    sps_format_t output_format;
    double alsa_maximum_stall_time;
    volatile int keep_dac_busy;
    yna_type use_precision_timing;
    */
    double set_volume;

    // set to true if there has been a discontinuity between the last reported frames_sent_for_playing
    // and the present reported frames_sent_for_playing
    // Note that it will be set when the device is opened, as any previous figures for
    // frames_sent_for_playing (which Shairport Sync might hold) would be invalid.
    int frames_sent_break_occurred;

    uint64_t frames_sent_for_playing;

    alsa_backend_mode alsa_backend_state;

    // use this to allow the use of standard or precision delay calculations, with standard the, uh,
    // standard.
    int (*delay_and_status)(alsa_output *output, snd_pcm_state_t *state, snd_pcm_sframes_t *delay, yndk_type *using_update_timestamps);

    int alsa_characteristics_already_listed; // = 0;
};

typedef struct
{
    // Pointer to array of alsa configurations
    alsa_output *outputs;
    // Count of how many outputs are in there.
    uint8_t output_count;

    // Debug output pointer for alsa
    snd_output_t *debug_output;
} alsa_outputs;

// Storage object for all outputs
static alsa_outputs alsa_outputs_g;

// this will return true if the DAC can return precision delay information and false if not
// if it is not yet known, it will test the output device to find out

// note -- once it has done the test, it decides -- even if the delay comes back with
// "don't know", it will take that as a "No" and remember it.
// If you want it to check again, set precision_delay_available_status to YNDK_DONT_KNOW
// first.

static int precision_delay_available(alsa_output *output)
{
    if (output->precision_delay_available_status == YNDK_DONT_KNOW)
    {
        // this is very crude -- if the device is a hardware device, then it's assumed the delay is
        // precise
        const char *output_device_name = snd_pcm_name(output->alsa_handle);
        int is_a_real_hardware_device = 0;
        if (output_device_name != NULL)
        {
            is_a_real_hardware_device = (strstr(output_device_name, "hw:") == output_device_name);
        }

        // The criteria as to whether precision delay is available
        // is whether the device driver returns non-zero update timestamps
        // If it does, and the device is a hardware device (i.e. its name begins with "hw:"),
        // it is considered that precision delay is available. Otherwise, it's considered to be
        // unavailable.

        // To test, we play a silence buffer (fairly large to avoid underflow)
        // and then we check the delay return. It will tell us if it
        // was able to use the (non-zero) update timestamps

        int frames_of_silence = 4410;
        size_t size_of_silence_buffer = frames_of_silence * output->frame_size;
        void *silence = malloc(size_of_silence_buffer);
        if (silence == NULL)
        {
            debug(1, "multi_alsa: precision_delay_available -- failed to "
                     "allocate memory for a "
                     "silent frame buffer.");
        }
        else
        {
            pthread_cleanup_push(malloc_cleanup, silence);
            int use_dither = 0;
            if ((output->alsa_mix_ctrl == NULL) && (config.ignore_volume_control == 0) && (config.airplay_volume != 0.0))
            {
                use_dither = 1;
            }
            output->dither_random_number_store =
                generate_zero_frames(silence, frames_of_silence, config.output_format,
                                     use_dither, // i.e. with dither
                                     output->dither_random_number_store);
            do_play(output, silence, frames_of_silence);
            pthread_cleanup_pop(1);
            // now we can get the delay, and we'll note if it uses update timestamps
            yndk_type uses_update_timestamps;
            snd_pcm_state_t state;
            snd_pcm_sframes_t delay;
            int ret = precision_delay_and_status(output, &state, &delay, &uses_update_timestamps);
            // debug(3,"alsa: precision_delay_available asking for delay and status with a return status
            // of %d, a delay of %ld and a uses_update_timestamps of %d.", ret, delay,
            // uses_update_timestamps);
            if (ret == 0)
            {
                if ((uses_update_timestamps == YNDK_YES) && (is_a_real_hardware_device))
                {
                    output->precision_delay_available_status = YNDK_YES;
                    debug(2, "multi_alsa: precision delay timing is available.");
                }
                else
                {
                    if ((uses_update_timestamps == YNDK_YES) && (!is_a_real_hardware_device))
                    {
                        debug(2, "multi_alsa: precision delay timing is not available because it's not definitely a "
                                 "hardware device.");
                    }
                    else
                    {
                        debug(2, "multi_alsa: precision delay timing is not available.");
                    }

                    output->precision_delay_available_status = YNDK_NO;
                }
            }
        }
    }

    return (output->precision_delay_available_status == YNDK_YES);
}

static void initialize_alsa_output(alsa_output *output)
{
    output->output_method_signalled = 0; // for reporting whether it's using mmap or not
    output->delay_type_notified = -1;    // for controlling the reporting of whether the output device can do
                                         // precision delays (e.g. alsa->pulsaudio virtual devices can't)
    output->use_monotonic_clock = 0;     // this value will be set when the hardware is initialised

    output->output = NULL;

    output->precision_delay_available_status = YNDK_DONT_KNOW; // initially, we don't know if the device can do precision delay

    output->alsa_handle = NULL;
    output->alsa_handle_status = ENODEV; // if alsa_handle is NULL, this should say why with a unix error code
    output->alsa_params = NULL;
    output->alsa_swparams = NULL;
    output->ctl = NULL;
    output->elem_id = NULL;
    output->alsa_mix_handle = NULL;
    output->alsa_mix_elem = NULL;
    output->alsa_mix_sid = NULL;

    output->alsa_out_dev = default_default;
    output->alsa_mix_dev = NULL;
    output->alsa_mix_ctrl = NULL;
    output->alsa_mix_index = 0;
    output->has_softvol = 0;

    output->dither_random_number_store = 0;

    output->volume_set_request = 0; // set when an external request is made to set the volume.

    output->mixer_volume_setting_gives_mute = 0; // set when it is discovered that
                                                 // particular mixer volume setting
                                                 // causes a mute.
    // output->alsa_mix_mute;                  // setting the volume to this value mutes output, if
    //  mixer_volume_setting_gives_mute is true
    output->volume_based_mute_is_active = 0; // set when muting is being done by a setting the volume to a magic value

    // use this to allow the use of snd_pcm_writei or snd_pcm_mmap_writei
    output->alsa_pcm_write = snd_pcm_writei;

    output->set_period_size_request = 0;
    output->set_buffer_size_request = 0;

    // output->use_precision_timing = YNA_AUTO;
    // output->keep_dac_busy = 0;
    // output->disable_standby_mode = disable_standby_off;
}

// Static Methods Implementations //

static void help(void)
{
    printf("    Provide a quote enclosed list for each option :\n"
           "    -d output-devices    set the output devices, default is one \"default\".\n"
           "    -c mixer-controls    set the mixer control name for outputs, default is to use no mixer.\n"
           "    -m mixer-devices     set the mixer device for outputs, default is the output device.\n"
           "    -i mixer-indexes     set the mixer index for outputs, default is 0.\n"
           "    -x delimiter         optional delimiter used to separate each output parameter in each option. default is \" \" ");

    int r = system("if [ -d /proc/asound ] ; then echo \"    hardware output devices:\" ; ls -al /proc/asound/ 2>/dev/null | grep '\\->' | tr -s ' ' | cut -d ' ' -f 9 | while read line; do echo \"      \\\"hw:$line\\\"\" ; done ; fi");
    if (r != 0)
    {
        debug(1, "error %d executing a script to list alsa hardware device names", r);
    }
}

// assuming pthread cancellation is disabled
// returns zero of all is okay, a Unx error code if there's a problem
static int open_mixer(alsa_output *output)
{
    int response = 0;
    if (output->alsa_mix_ctrl != NULL)
    {
        debug(3, "Open Mixer %s", output->alsa_mix_ctrl);
        snd_mixer_selem_id_alloca(&output->alsa_mix_sid);
        snd_mixer_selem_id_set_index(output->alsa_mix_sid, output->alsa_mix_index);
        snd_mixer_selem_id_set_name(output->alsa_mix_sid, output->alsa_mix_ctrl);

        if ((response = snd_mixer_open(&output->alsa_mix_handle, 0)) < 0)
        {
            debug(1, "Failed to open mixer");
        }
        else
        {
            debug(3, "Mixer device name is \"%s\".", output->alsa_mix_dev);
            if ((response = snd_mixer_attach(output->alsa_mix_handle, output->alsa_mix_dev)) < 0)
            {
                debug(1, "Failed to attach mixer");
            }
            else
            {
                if ((response = snd_mixer_selem_register(output->alsa_mix_handle, NULL, NULL)) < 0)
                {
                    debug(1, "Failed to register mixer element");
                }
                else
                {
                    if ((response = snd_mixer_load(output->alsa_mix_handle)) < 0)
                    {
                        debug(1, "Failed to load mixer element");
                    }
                    else
                    {
                        debug(3, "Mixer control is \"%s\",%d.", output->alsa_mix_ctrl, output->alsa_mix_index);
                        output->alsa_mix_elem = snd_mixer_find_selem(output->alsa_mix_handle, output->alsa_mix_sid);
                        if (!output->alsa_mix_elem)
                        {
                            warn("failed to find mixer control \"%s\",%d.", output->alsa_mix_ctrl, output->alsa_mix_index);
                            response = -ENXIO; // don't let this be ENODEV!
                        }
                    }
                }
            }
        }
    }

    return response;
}

// assuming pthread cancellation is disabled
static int close_mixer(alsa_output *output)
{
    int response = 0;
    if (output->alsa_mix_handle)
    {
        response = snd_mixer_close(output->alsa_mix_handle);
        output->alsa_mix_handle = NULL;
    }

    return response;
}

// assuming pthread cancellation is disabled
static int do_snd_mixer_selem_set_playback_dB_all(snd_mixer_elem_t *mix_elem, double vol)
{
    int response = 0;
    if ((response = snd_mixer_selem_set_playback_dB_all(mix_elem, vol, 0)) != 0)
    {
        debug(1, "Can't set playback volume accurately to %f dB.", vol);
        if ((response = snd_mixer_selem_set_playback_dB_all(mix_elem, vol, -1)) != 0)
        {
            if ((response = snd_mixer_selem_set_playback_dB_all(mix_elem, vol, 1)) != 0)
            {
                debug(1, "Could not set playback dB volume on the mixer.");
            }
        }
    }

    return response;
}

// assuming pthread cancellation is disabled
// if do_auto_setting is true and auto format or auto speed has been requested,
// select the settings as appropriate and store them
static int actual_open_alsa_device(alsa_output *output, int do_auto_setup)
{
    // the alsa mutex is already acquired when this is called
    const snd_pcm_uframes_t minimal_buffer_headroom =
        352 * 2; // we accept this much headroom in the hardware buffer, but we'll
                 // accept less
                 /*
                     const snd_pcm_uframes_t requested_buffer_headroom =
                         minimal_buffer_headroom + 2048; // we ask for this much headroom in the
                                                         // hardware buffer, but we'll accept
                     less
                 */

    int ret, dir = 0;
    unsigned int actual_sample_rate; // this will be given the rate requested and will be given the actual rate
    // snd_pcm_uframes_t frames = 441 * 10;
    snd_pcm_uframes_t actual_buffer_length;
    snd_pcm_access_t access;

    // ensure no calls are made to the alsa device enquiring about the buffer
    // length if
    // synchronisation is disabled.
    if (config.no_sync != 0)
    {
        audio_multi_alsa.delay = NULL;
    }

    ret = snd_pcm_open(&output->alsa_handle, output->alsa_out_dev, SND_PCM_STREAM_PLAYBACK, 0);
    // EHOSTDOWN seems to signify that it's a PipeWire pseudo device that can't be accessed by this
    // user. So, try the first device ALSA device and log it.
    if ((ret == -EHOSTDOWN) && (strcmp(output->alsa_out_dev, "default") == 0))
    {
        ret = snd_pcm_open(&output->alsa_handle, "hw:0", SND_PCM_STREAM_PLAYBACK, 0);
        if ((ret == 0) || (ret == -EBUSY))
        {
            // being busy should be okay
            inform("the default ALSA device is inaccessible -- \"hw:0\" used instead.", output->alsa_out_dev);
            output->alsa_out_dev = "hw:0";
        }
    }

    if (ret == 0)
    {
        if (output->alsa_handle_status == -EBUSY)
        {
            warn("The output device \"%s\" is no longer busy and will be used by Shairport Sync.", output->alsa_out_dev);
            output->alsa_handle_status = ret; // all cool
        }
    }
    else
    {
        output->alsa_handle = NULL; // to be sure to be sure
        if (ret == -EBUSY)
        {
            if (output->alsa_handle_status != -EBUSY)
            {
                warn("The output device \"%s\" is busy and can't be used by Shairport Sync at present.", output->alsa_out_dev);
            }

            debug(2, "the multi alsa output_device \"%s\" is busy.", output->alsa_out_dev);
        }

        output->alsa_handle_status = ret;
        output->frames_sent_break_occurred = 1;
        return ret;
    }

    snd_pcm_hw_params_alloca(&output->alsa_params);
    snd_pcm_sw_params_alloca(&output->alsa_swparams);

    ret = snd_pcm_hw_params_any(output->alsa_handle, output->alsa_params);
    if (ret < 0)
    {
        die("audio_multi_alsa: Broken configuration for device \"%s\": no configurations "
            "available",
            output->alsa_out_dev);
        return ret;
    }

    if ((config.no_mmap == 0) && (snd_pcm_hw_params_set_access(output->alsa_handle, output->alsa_params, SND_PCM_ACCESS_MMAP_INTERLEAVED) >= 0))
    {
        if (output->output_method_signalled == 0)
        {
            debug(3, "Output written using MMAP");
            output->output_method_signalled = 1;
        }
        access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
        output->alsa_pcm_write = snd_pcm_mmap_writei;
    }
    else
    {
        if (output->output_method_signalled == 0)
        {
            debug(3, "Output written with RW");
            output->output_method_signalled = 1;
        }
        access = SND_PCM_ACCESS_RW_INTERLEAVED;
        output->alsa_pcm_write = snd_pcm_writei;
    }

    ret = snd_pcm_hw_params_set_access(output->alsa_handle, output->alsa_params, access);
    if (ret < 0)
    {
        die("audio_multi_alsa: Access type not available for device \"%s\": %s", output->alsa_out_dev,
            snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_hw_params_set_channels(output->alsa_handle, output->alsa_params, 2);
    if (ret < 0)
    {
        die("audio_multi_alsa: Channels count (2) not available for device \"%s\": %s", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    snd_pcm_format_t sf;

    if ((do_auto_setup == 0) || (config.output_format_auto_requested == 0)) // no auto format
    {
        if ((config.output_format > SPS_FORMAT_UNKNOWN) && (config.output_format < SPS_FORMAT_AUTO))
        {
            sf = fr[config.output_format].alsa_code;
            output->frame_size = fr[config.output_format].frame_size;
        }
        else
        {
            warn("multi_alsa: unexpected output format %d. Set to S16_LE.", config.output_format);
            config.output_format = SPS_FORMAT_S16_LE;
            sf = fr[config.output_format].alsa_code;
            output->frame_size = fr[config.output_format].frame_size;
        }
        ret = snd_pcm_hw_params_set_format(output->alsa_handle, output->alsa_params, sf);
        if (ret < 0)
        {
            die("audio_multi_alsa: Alsa sample format %d not available for device \"%s\": %s", sf, output->alsa_out_dev, snd_strerror(ret));
            return ret;
        }
    }
    else
    { // auto format
        sps_format_t *formats;
        formats = auto_format_check_sequence;
        int i = 0;
        int format_found = 0;
        sps_format_t trial_format = SPS_FORMAT_UNKNOWN;
        while ((i < number_sps_format_t) && (format_found == 0))
        {
            trial_format = formats[i];
            sf = fr[trial_format].alsa_code;
            output->frame_size = fr[trial_format].frame_size;
            ret = snd_pcm_hw_params_set_format(output->alsa_handle, output->alsa_params, sf);
            if (ret == 0)
            {
                format_found = 1;
            }
            else
            {
                i++;
            }
        }
        if (ret == 0)
        {
            config.output_format = trial_format;
            debug(2, "multi_alsa: output format chosen is \"%s\".",
                  sps_format_description_string(config.output_format));
        }
        else
        {
            die("audio_alsa: Could not automatically set the output format for device \"%s\": %s",
                output->alsa_out_dev, snd_strerror(ret));
            return ret;
        }
    }

    if ((do_auto_setup == 0) || (config.output_rate_auto_requested == 0)) // no auto format
    {
        actual_sample_rate = config.output_rate; // this is the requested rate -- it'll be changed to the actual rate
        ret = snd_pcm_hw_params_set_rate_near(output->alsa_handle, output->alsa_params, &actual_sample_rate, &dir);
        if (ret < 0)
        {
            die("audio_alsa: The frame rate of %i frames per second is not available for playback: %s", config.output_rate, snd_strerror(ret));
            return ret;
        }
    }
    else
    {
        unsigned int *speeds;

        speeds = auto_speed_output_rates;
        int i = 0;
        int speed_found = 0;

        while ((i < number_speeds) && (speed_found == 0))
        {
            actual_sample_rate = speeds[i];
            ret = snd_pcm_hw_params_set_rate_near(output->alsa_handle, output->alsa_params, &actual_sample_rate, &dir);
            if (ret == 0)
            {
                speed_found = 1;
                if (actual_sample_rate != speeds[i])
                {
                    die("The output DAC can not be set to %d frames per second (fps). The nearest speed "
                        "available is %d fps.",
                        speeds[i], actual_sample_rate);
                }
            }
            else
            {
                i++;
            }
        }
        if (ret == 0)
        {
            config.output_rate = actual_sample_rate;
            debug(2, "multi_alsa: output speed chosen is %d.", config.output_rate);
        }
        else
        {
            die("audio_alsa: Could not automatically set the output rate for device \"%s\": %s",
                output->alsa_out_dev, snd_strerror(ret));
            return ret;
        }
    }

    if (output->set_period_size_request != 0)
    {
        debug(1, "Attempting to set the period size to %lu", output->period_size_requested);
        ret = snd_pcm_hw_params_set_period_size_near(output->alsa_handle, output->alsa_params, &output->period_size_requested, &dir);
        if (ret < 0)
        {
            warn("audio_alsa: cannot set period size of %lu: %s", output->period_size_requested, snd_strerror(ret));
            return ret;
        }
        else
        {
            snd_pcm_uframes_t actual_period_size;
            snd_pcm_hw_params_get_period_size(output->alsa_params, &actual_period_size, &dir);
            if (actual_period_size != output->period_size_requested)
            {
                inform("Actual period size set to a different value than requested. "
                       "Requested: %lu, actual "
                       "setting: %lu",
                       output->period_size_requested, actual_period_size);
            }
        }
    }

    if (output->set_buffer_size_request != 0)
    {
        debug(1, "Attempting to set the buffer size to %lu", output->buffer_size_requested);
        ret = snd_pcm_hw_params_set_buffer_size_near(output->alsa_handle, output->alsa_params, &output->buffer_size_requested);
        if (ret < 0)
        {
            warn("audio_alsa: cannot set buffer size of %lu: %s", output->buffer_size_requested,
                 snd_strerror(ret));
            return ret;
        }
        else
        {
            snd_pcm_uframes_t actual_buffer_size;
            snd_pcm_hw_params_get_buffer_size(output->alsa_params, &actual_buffer_size);
            if (actual_buffer_size != output->buffer_size_requested)
            {
                inform("Actual period size set to a different value than requested. "
                       "Requested: %lu, actual "
                       "setting: %lu",
                       output->buffer_size_requested, actual_buffer_size);
            }
        }
    }

    ret = snd_pcm_hw_params(output->alsa_handle, output->alsa_params);
    if (ret < 0)
    {
        die("audio_alsa: Unable to set hw parameters for device \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    // check parameters after attempting to set them

    if (output->set_period_size_request != 0)
    {
        snd_pcm_uframes_t actual_period_size;
        snd_pcm_hw_params_get_period_size(output->alsa_params, &actual_period_size, &dir);
        if (actual_period_size != output->period_size_requested)
        {
            inform("Actual period size set to a different value than requested. "
                   "Requested: %lu, actual "
                   "setting: %lu",
                   output->period_size_requested, actual_period_size);
        }
    }

    if (output->set_buffer_size_request != 0)
    {
        snd_pcm_uframes_t actual_buffer_size;
        snd_pcm_hw_params_get_buffer_size(output->alsa_params, &actual_buffer_size);
        if (actual_buffer_size != output->buffer_size_requested)
        {
            inform("Actual period size set to a different value than requested. "
                   "Requested: %lu, actual "
                   "setting: %lu",
                   output->buffer_size_requested, actual_buffer_size);
        }
    }

    if (actual_sample_rate != config.output_rate)
    {
        die("Can't set the output DAC to the requested frame rate of %d fps.", config.output_rate);
        return -EINVAL;
    }

    output->use_monotonic_clock = snd_pcm_hw_params_is_monotonic(output->alsa_params);

    ret = snd_pcm_hw_params_get_buffer_size(output->alsa_params, &actual_buffer_length);
    if (ret < 0)
    {
        warn("audio_alsa: Unable to get hw buffer length for device \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_sw_params_current(output->alsa_handle, output->alsa_swparams);
    if (ret < 0)
    {
        warn("audio_alsa: Unable to get current sw parameters for device \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_sw_params_set_tstamp_mode(output->alsa_handle, output->alsa_swparams, SND_PCM_TSTAMP_ENABLE);
    if (ret < 0)
    {
        warn("audio_alsa: Can't enable timestamp mode of device: \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    /* write the sw parameters */
    ret = snd_pcm_sw_params(output->alsa_handle, output->alsa_swparams);
    if (ret < 0)
    {
        warn("audio_alsa: Unable to set software parameters of device: \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    ret = snd_pcm_prepare(output->alsa_handle);
    if (ret < 0)
    {
        warn("audio_alsa: Unable to prepare the device: \"%s\": %s.", output->alsa_out_dev, snd_strerror(ret));
        return ret;
    }

    if (actual_buffer_length < config.audio_backend_buffer_desired_length + minimal_buffer_headroom)
    {
        /*
        // the dac buffer is too small, so let's try to set it
        buffer_size =
            config.audio_backend_buffer_desired_length + requested_buffer_headroom;
        ret = snd_pcm_hw_params_set_buffer_size_near(alsa_handle, alsa_params,
                                                    &buffer_size);
        if (ret < 0)
        die("audio_alsa: Unable to set hw buffer size to %lu for device \"%s\": "
            "%s.",
            config.audio_backend_buffer_desired_length +
                requested_buffer_headroom,
            alsa_out_dev, snd_strerror(ret));
        if (config.audio_backend_buffer_desired_length + minimal_buffer_headroom >
            buffer_size) {
        die("audio_alsa: Can't set hw buffer size to %lu or more for device "
            "\"%s\". Requested size: %lu, granted size: %lu.",
            config.audio_backend_buffer_desired_length + minimal_buffer_headroom,
            alsa_out_dev, config.audio_backend_buffer_desired_length +
                                requested_buffer_headroom,
            buffer_size);
        }
        */
        debug(1,
              "The alsa buffer is smaller (%lu bytes) than the desired backend "
              "buffer "
              "length (%ld) you have chosen.",
              actual_buffer_length, config.audio_backend_buffer_desired_length);
    }

    if (config.use_precision_timing == YNA_YES)
    {
        output->delay_and_status = precision_delay_and_status;
    }
    else if (config.use_precision_timing == YNA_AUTO)
    {
        if (precision_delay_available(output))
        {
            output->delay_and_status = precision_delay_and_status;
            debug(2, "multi_alsa: precision timing selected for \"auto\" mode");
        }
    }

    if (output->alsa_characteristics_already_listed == 0)
    {
        output->alsa_characteristics_already_listed = 1;
        int log_level = 2; // the level at which debug information should be output
                           //    int rc;
        snd_pcm_access_t access_type;
        snd_pcm_format_t format_type;
        snd_pcm_subformat_t subformat_type;
        //    unsigned int val, val2;
        unsigned int uval, uval2;
        int sval;
        int dir;
        snd_pcm_uframes_t frames;

        debug(log_level, "PCM handle name = '%s'", snd_pcm_name(output->alsa_handle));

        //      ret = snd_pcm_hw_params_any(alsa_handle, alsa_params);
        //      if (ret < 0) {
        //        die("audio_alsa: Cannpot get configuration for
        // device
        //\"%s\":
        // no
        // configurations
        //"
        //            "available",
        //            alsa_out_dev);
        //      }

        debug(log_level, "multi_alsa device parameters:");

        snd_pcm_hw_params_get_access(output->alsa_params, &access_type);
        debug(log_level, "  access type = %s", snd_pcm_access_name(access_type));

        snd_pcm_hw_params_get_format(output->alsa_params, &format_type);
        debug(log_level, "  format = '%s' (%s)", snd_pcm_format_name(format_type),
              snd_pcm_format_description(format_type));

        snd_pcm_hw_params_get_subformat(output->alsa_params, &subformat_type);
        debug(log_level, "  subformat = '%s' (%s)", snd_pcm_subformat_name(subformat_type),
              snd_pcm_subformat_description(subformat_type));

        snd_pcm_hw_params_get_channels(output->alsa_params, &uval);
        debug(log_level, "  number of channels = %u", uval);

        sval = snd_pcm_hw_params_get_sbits(output->alsa_params);
        debug(log_level, "  number of significant bits = %d", sval);

        snd_pcm_hw_params_get_rate(output->alsa_params, &uval, &dir);
        switch (dir)
        {
        case -1:
            debug(log_level, "  rate = %u frames per second (<).", uval);
            break;
        case 0:
            debug(log_level, "  rate = %u frames per second (precisely).", uval);
            break;
        case 1:
            debug(log_level, "  rate = %u frames per second (>).", uval);
            break;
        }

        if ((snd_pcm_hw_params_get_rate_numden(output->alsa_params, &uval, &uval2) == 0) && (uval2 != 0))
        {
            // watch for a divide by zero too!
            debug(log_level, "  precise (rational) rate = %.3f frames per second (i.e. %u/%u).", uval,
                  uval2, ((double)uval) / uval2);
        }
        else
        {
            debug(log_level, "  precise (rational) rate information unavailable.");
        }

        snd_pcm_hw_params_get_period_time(output->alsa_params, &uval, &dir);
        switch (dir)
        {
        case -1:
            debug(log_level, "  period_time = %u us (<).", uval);
            break;
        case 0:
            debug(log_level, "  period_time = %u us (precisely).", uval);
            break;
        case 1:
            debug(log_level, "  period_time = %u us (>).", uval);
            break;
        }

        snd_pcm_hw_params_get_period_size(output->alsa_params, &frames, &dir);
        switch (dir)
        {
        case -1:
            debug(log_level, "  period_size = %lu frames (<).", frames);
            break;
        case 0:
            debug(log_level, "  period_size = %lu frames (precisely).", frames);
            break;
        case 1:
            debug(log_level, "  period_size = %lu frames (>).", frames);
            break;
        }

        snd_pcm_hw_params_get_buffer_time(output->alsa_params, &uval, &dir);
        switch (dir)
        {
        case -1:
            debug(log_level, "  buffer_time = %u us (<).", uval);
            break;
        case 0:
            debug(log_level, "  buffer_time = %u us (precisely).", uval);
            break;
        case 1:
            debug(log_level, "  buffer_time = %u us (>).", uval);
            break;
        }

        snd_pcm_hw_params_get_buffer_size(output->alsa_params, &frames);
        switch (dir)
        {
        case -1:
            debug(log_level, "  buffer_size = %lu frames (<).", frames);
            break;
        case 0:
            debug(log_level, "  buffer_size = %lu frames (precisely).", frames);
            break;
        case 1:
            debug(log_level, "  buffer_size = %lu frames (>).", frames);
            break;
        }

        snd_pcm_hw_params_get_periods(output->alsa_params, &uval, &dir);
        switch (dir)
        {
        case -1:
            debug(log_level, "  periods_per_buffer = %u (<).", uval);
            break;
        case 0:
            debug(log_level, "  periods_per_buffer = %u (precisely).", uval);
            break;
        case 1:
            debug(log_level, "  periods_per_buffer = %u (>).", uval);
            break;
        }
    }

    return 0;
}

static int open_alsa_device(alsa_output *output, int do_auto_setup)
{
    int result;
    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
    result = actual_open_alsa_device(output, do_auto_setup);
    pthread_setcancelstate(oldState, NULL);
    return result;
}

static int prepare_mixer(alsa_output *output)
{
    int response = 0;
    // do any alsa device initialisation (general case)
    // at present, this is only needed if a hardware mixer is being used
    // if there's a hardware mixer, it needs to be initialised before use
    if (output->alsa_mix_ctrl != NULL)
    {
        debug(2, "multi_alsa: hardware mixer prepare %s", output->alsa_out_dev);
        int oldState;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
        if (output->alsa_mix_dev == NULL)
        {
            output->alsa_mix_dev = output->alsa_out_dev;
        }

        // Now, start trying to initialise the alsa device with the settings
        // obtained
        pthread_cleanup_debug_mutex_lock(&output->alsa_mixer_mutex, 1000, 1);

        if (open_mixer(output) == 0)
        {
            if (snd_mixer_selem_get_playback_volume_range(output->alsa_mix_elem, &output->alsa_mix_minv, &output->alsa_mix_maxv) < 0)
            {
                debug(1, "Can't read mixer's [linear] min and max volumes.");
            }
            else
            {
                if (snd_mixer_selem_get_playback_dB_range(output->alsa_mix_elem, &output->alsa_mix_mindb, &output->alsa_mix_maxdb) == 0)
                {
                    audio_multi_alsa.volume = &volume;         // insert the volume function now we
                                                               // know it can do dB stuff
                    audio_multi_alsa.parameters = &parameters; // likewise the parameters stuff
                    if (output->alsa_mix_mindb == SND_CTL_TLV_DB_GAIN_MUTE)
                    {
                        // For instance, the Raspberry Pi does this
                        debug(2, "Lowest dB value is a mute");
                        output->mixer_volume_setting_gives_mute = 1;
                        output->alsa_mix_mute = SND_CTL_TLV_DB_GAIN_MUTE; // this may not be
                                                                          // necessary -- it's
                                                                          // always
                        // going to be SND_CTL_TLV_DB_GAIN_MUTE, right?
                        // debug(1, "Try minimum volume + 1 as lowest true attenuation
                        // value");
                        if (snd_mixer_selem_ask_playback_vol_dB(output->alsa_mix_elem, output->alsa_mix_minv + 1, &output->alsa_mix_mindb) != 0)
                        {
                            debug(1, "Can't get dB value corresponding to a minimum volume + 1.");
                        }
                    }

                    debug(3, "Hardware mixer has dB volume from %f to %f.", (1.0 * output->alsa_mix_mindb) / 100.0, (1.0 * output->alsa_mix_maxdb) / 100.0);
                }
                else
                {
                    // use the linear scale and do the db conversion ourselves
                    warn("The hardware mixer specified -- \"%s\" -- does not have "
                         "a dB volume scale.",
                         output->alsa_mix_ctrl);

                    if ((response = snd_ctl_open(&output->ctl, output->alsa_mix_dev, 0)) < 0)
                    {
                        warn("Cannot open control \"%s\"", output->alsa_mix_dev);
                    }
                    if ((response = snd_ctl_elem_id_malloc(&output->elem_id)) < 0)
                    {
                        debug(1, "Cannot allocate memory for control \"%s\"", output->alsa_mix_dev);
                        output->elem_id = NULL;
                    }
                    else
                    {
                        snd_ctl_elem_id_set_interface(output->elem_id, SND_CTL_ELEM_IFACE_MIXER);
                        snd_ctl_elem_id_set_name(output->elem_id, output->alsa_mix_ctrl);

                        if (snd_ctl_get_dB_range(output->ctl, output->elem_id, &output->alsa_mix_mindb, &output->alsa_mix_maxdb) == 0)
                        {
                            debug(1,
                                  "multi_alsa: hardware mixer \"%s\" selected, with dB volume "
                                  "from %f to %f.",
                                  output->alsa_mix_ctrl, (1.0 * output->alsa_mix_mindb) / 100.0, (1.0 * output->alsa_mix_maxdb) / 100.0);
                            output->has_softvol = 1;
                            audio_multi_alsa.volume = &volume;         // insert the volume function now
                                                                       // we know it can do dB stuff
                            audio_multi_alsa.parameters = &parameters; // likewise the parameters stuff
                        }
                        else
                        {
                            debug(1, "Cannot get the dB range from the volume control \"%s\"", output->alsa_mix_ctrl);
                        }
                    }
                }
            }

            if (((config.alsa_use_hardware_mute == 1) && (snd_mixer_selem_has_playback_switch(output->alsa_mix_elem))) ||
                output->mixer_volume_setting_gives_mute)
            {
                audio_multi_alsa.mute = &mute; // insert the mute function now we know it
                                               // can do muting stuff
                // debug(1, "Has mixer and mute ability we will use.");
            }
            else
            {
                // debug(1, "Has mixer but not using hardware mute.");
            }

            if (response == 0)
            {
                response = close_mixer(output);
            }
        }

        debug_mutex_unlock(&output->alsa_mixer_mutex, 3); // release the mutex
        pthread_cleanup_pop(0);
        pthread_setcancelstate(oldState, NULL);
    }

    return response;
}

static int alsa_device_init(alsa_output *output)
{
    return prepare_mixer(output);
}

static int init(int argc, char **argv)
{
    // Initialize the alsa_outputs_g struct to all zeros.
    memset(&alsa_outputs_g, 0, sizeof(alsa_outputs));

    // For debugging
    snd_output_stdio_attach(&alsa_outputs_g.debug_output, stdout, 0);

    // debug(2, "audio_multi_alsa init called.");
    int response = 0; // This will be what is returned to the caller
    const char *str;
    int value;

    config.alsa_use_hardware_mute = 0; // don't use it by default

    debug(2, "multi_alsa: init() -- alsa_backend_state => abm_disconnected.");

    config.audio_backend_latency_offset = 0;
    config.audio_backend_buffer_desired_length = 0.200;
    config.audio_backend_buffer_interpolation_threshold_in_seconds =
        0.120;                              // below this, basic interpolation will be used to save time.
    config.alsa_maximum_stall_time = 0.200; // 200 milliseconds -- if it takes longer, it's a problem
    config.disable_standby_mode_silence_threshold =
        0.040;                                                 // start sending silent frames if the delay goes below this time
    config.disable_standby_mode_silence_scan_interval = 0.004; // check silence threshold this often

    uint64_t temp_stall_monitor_error_threshold = (uint64_t)1000000 * config.alsa_maximum_stall_time; // stall time max to microseconds;
    temp_stall_monitor_error_threshold = (temp_stall_monitor_error_threshold << 32) / 1000000;        // now in fp form
    debug(1, "multi_alsa: alsa_maximum_stall_time of %f sec.", config.alsa_maximum_stall_time);

    config.disable_standby_mode = disable_standby_off;
    config.keep_dac_busy = 0;
    config.use_precision_timing = YNA_AUTO;

    // get settings from settings file first, allow them to be overridden by
    // command line options

    // do the "general" audio  options. Note, these options are in the "general"
    // stanza!
    parse_general_audio_options();

    if (config.cfg != NULL)
    {
        double dvalue;
        unsigned int icountvalue = 0;
        config_setting_t *settings_outs;

        // Find the block for multi_alsa
        settings_outs = config_lookup(config.cfg, "multi_alsa.outputs");
        if (settings_outs != NULL)
        {
            unsigned int idx;
            if (!config_setting_is_list(settings_outs))
            {
                die("audio_multi_alsa: init() : multi_alsa.outputs from the configuration is not a list");
            }

            icountvalue = config_setting_length(settings_outs);
            debug(2, "audio_multi_alsa: init() : multi_alsa.outputs length is %d ", icountvalue);
            if (icountvalue > 0)
            {
                alsa_outputs_g.outputs = (alsa_output *)malloc(icountvalue * sizeof(alsa_output));
                if (alsa_outputs_g.outputs == NULL)
                {
                    die("audio_multi_alsa: init() : Failed to allocate memory of %d for alsa_outputs."
                        " The alsa_outputs pointer is NULL.",
                        icountvalue * sizeof(alsa_output));
                }

                alsa_outputs_g.output_count = (uint8_t)icountvalue;

                for (idx = 0; idx < icountvalue; idx++)
                {
                    memset(&alsa_outputs_g.outputs[idx], 0, sizeof(alsa_output));
                    initialize_alsa_output(&alsa_outputs_g.outputs[idx]);
                    alsa_outputs_g.outputs[idx].alsa_device_initialised = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_error_threshold = temp_stall_monitor_error_threshold;
                    alsa_outputs_g.outputs[idx].stall_monitor_start_time = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_frame_count = 0;
                    alsa_outputs_g.outputs[idx].alsa_backend_state = abm_disconnected; // startup state
                    alsa_outputs_g.outputs[idx].delay_and_status = standard_delay_and_status;
                    alsa_outputs_g.outputs[idx].alsa_characteristics_already_listed = 0;
                }

                for (idx = 0; idx < icountvalue; idx++)
                {

                    config_setting_t *an_out = config_setting_get_elem(settings_outs, idx);
                    if (an_out == NULL)
                    {
                        die("audio_multi_alsa: init() : could not retieve multi_alsa output at index %d", idx);
                    }

                    // Get the Output Device Name : output_device
                    if (config_setting_lookup_string(an_out, "output_device", &str))
                    {
                        alsa_outputs_g.outputs[idx].alsa_out_dev = (char *)str;
                        debug(2, "audio_multi_alsa: init() : multi_alsa.outputs output_device for "
                                 "index %d is %s ",
                              idx, alsa_outputs_g.outputs[idx].alsa_out_dev);
                    }

                    // Get the Mixer Type setting : mixer_type
                    if (config_setting_lookup_string(an_out, "mixer_type", &str))
                    {
                        inform("The alsa mixer_type setting is deprecated and has been ignored. "
                               "FYI, using the \"mixer_control_name\" setting automatically "
                               "chooses a hardware mixer.");
                    }

                    // Get the Mixer Device Name : mixer_device
                    if (config_setting_lookup_string(an_out, "mixer_device", &str))
                    {
                        alsa_outputs_g.outputs[idx].alsa_mix_dev = (char *)str;
                        debug(2, "audio_multi_alsa: init() : multi_alsa.outputs mixer_device for "
                                 "index %d is %s ",
                              idx, alsa_outputs_g.outputs[idx].alsa_mix_dev);
                    }

                    // Get the Mixer Control Name : mixer_control_name
                    if (config_setting_lookup_string(an_out, "mixer_control_name", &str))
                    {
                        alsa_outputs_g.outputs[idx].alsa_mix_ctrl = (char *)str;
                        debug(2, "audio_multi_alsa: init() : multi_alsa.outputs mixer_control_name for "
                                 "index %d is %s ",
                              idx, alsa_outputs_g.outputs[idx].alsa_mix_ctrl);
                    }

                    // Get the Mixer Control index : mixer_control_index
                    if (config_setting_lookup_int(an_out, "mixer_control_index", &value))
                    {
                        alsa_outputs_g.outputs[idx].alsa_mix_index = value;
                        debug(2, "audio_multi_alsa: init() : multi_alsa.outputs alsa_mix_index for "
                                 "index %d is %d ",
                              idx, alsa_outputs_g.outputs[idx].alsa_mix_index);
                    }
                }
            }
        }
        else
        {
            inform("No multi_alsa.outputs section found in the configuration");
        }

        /* Get the disable_synchronization setting. */
        if (config_lookup_string(config.cfg, "multi_alsa.disable_synchronization", &str))
        {
            if (strcasecmp(str, "no") == 0)
                config.no_sync = 0;
            else if (strcasecmp(str, "yes") == 0)
                config.no_sync = 1;
            else
            {
                warn("Invalid disable_synchronization option choice \"%s\". It should "
                     "be \"yes\" or "
                     "\"no\". It is set to \"no\".");
                config.no_sync = 0;
            }
        }

        /* Get the mute_using_playback_switch setting. */
        if (config_lookup_string(config.cfg, "multi_alsa.mute_using_playback_switch", &str))
        {
            inform("The alsa \"mute_using_playback_switch\" setting is deprecated. "
                   "Please use the \"use_hardware_mute_if_available\" setting instead.");
            if (strcasecmp(str, "no") == 0)
                config.alsa_use_hardware_mute = 0;
            else if (strcasecmp(str, "yes") == 0)
                config.alsa_use_hardware_mute = 1;
            else
            {
                warn("Invalid mute_using_playback_switch option choice \"%s\". It "
                     "should be \"yes\" or "
                     "\"no\". It is set to \"no\".");
                config.alsa_use_hardware_mute = 0;
            }
        }

        /* Get the use_hardware_mute_if_available setting. */
        if (config_lookup_string(config.cfg, "multi_alsa.use_hardware_mute_if_available", &str))
        {
            if (strcasecmp(str, "no") == 0)
                config.alsa_use_hardware_mute = 0;
            else if (strcasecmp(str, "yes") == 0)
                config.alsa_use_hardware_mute = 1;
            else
            {
                warn("Invalid use_hardware_mute_if_available option choice \"%s\". It "
                     "should be \"yes\" or "
                     "\"no\". It is set to \"no\".");
                config.alsa_use_hardware_mute = 0;
            }
        }

        /* Get the output format, using the same names as aplay does*/
        if (config_lookup_string(config.cfg, "multi_alsa.output_format", &str))
        {
            int temp_output_format_auto_requested = config.output_format_auto_requested;
            config.output_format_auto_requested = 0; // assume a valid format will be given.
            if (strcasecmp(str, "S16") == 0)
                config.output_format = SPS_FORMAT_S16;
            else if (strcasecmp(str, "S16_LE") == 0)
                config.output_format = SPS_FORMAT_S16_LE;
            else if (strcasecmp(str, "S16_BE") == 0)
                config.output_format = SPS_FORMAT_S16_BE;
            else if (strcasecmp(str, "S24") == 0)
                config.output_format = SPS_FORMAT_S24;
            else if (strcasecmp(str, "S24_LE") == 0)
                config.output_format = SPS_FORMAT_S24_LE;
            else if (strcasecmp(str, "S24_BE") == 0)
                config.output_format = SPS_FORMAT_S24_BE;
            else if (strcasecmp(str, "S24_3LE") == 0)
                config.output_format = SPS_FORMAT_S24_3LE;
            else if (strcasecmp(str, "S24_3BE") == 0)
                config.output_format = SPS_FORMAT_S24_3BE;
            else if (strcasecmp(str, "S32") == 0)
                config.output_format = SPS_FORMAT_S32;
            else if (strcasecmp(str, "S32_LE") == 0)
                config.output_format = SPS_FORMAT_S32_LE;
            else if (strcasecmp(str, "S32_BE") == 0)
                config.output_format = SPS_FORMAT_S32_BE;
            else if (strcasecmp(str, "U8") == 0)
                config.output_format = SPS_FORMAT_U8;
            else if (strcasecmp(str, "S8") == 0)
                config.output_format = SPS_FORMAT_S8;
            else if (strcasecmp(str, "auto") == 0)
                config.output_format_auto_requested = 1;
            else
            {
                config.output_format_auto_requested =
                    temp_output_format_auto_requested; // format was invalid; recall the original setting
                warn("Invalid output format \"%s\". It should be \"auto\", \"U8\", \"S8\", "
                     "\"S16\", \"S24\", \"S24_LE\", \"S24_BE\", "
                     "\"S24_3LE\", \"S24_3BE\" or "
                     "\"S32\", \"S32_LE\", \"S32_BE\". It remains set to \"%s\".",
                     str,
                     config.output_format_auto_requested == 1
                         ? "auto"
                         : sps_format_description_string(config.output_format));
            }
        }

        if (config_lookup_string(config.cfg, "multi_alsa.output_rate", &str))
        {
            if (strcasecmp(str, "auto") == 0)
            {
                config.output_rate_auto_requested = 1;
            }
            else
            {
                if (config.output_rate_auto_requested == 1)
                    warn("Invalid output rate \"%s\". It should be \"auto\", 44100, 88200, 176400 or 352800. "
                         "It remains set to \"auto\". Note: numbers should not be placed in quotes.",
                         str);
                else
                    warn("Invalid output rate \"%s\". It should be \"auto\", 44100, 88200, 176400 or 352800. "
                         "It remains set to %d. Note: numbers should not be placed in quotes.",
                         str, config.output_rate);
            }
        }

        /* Get the output rate, which must be a multiple of 44,100*/
        if (config_lookup_int(config.cfg, "multi_alsa.output_rate", &value))
        {
            debug(1, "alsa output rate is %d frames per second", value);
            switch (value)
            {
            case 44100:
            case 88200:
            case 176400:
            case 352800:
                config.output_rate = value;
                config.output_rate_auto_requested = 0;
                break;
            default:
                if (config.output_rate_auto_requested == 1)
                    warn("Invalid output rate \"%d\". It should be \"auto\", 44100, 88200, 176400 or 352800. "
                         "It remains set to \"auto\".",
                         value);
                else
                    warn("Invalid output rate \"%d\".It should be \"auto\", 44100, 88200, 176400 or 352800. "
                         "It remains set to %d.",
                         value, config.output_rate);
            }
        }

        /* Get the use_mmap_if_available setting. */
        if (config_lookup_string(config.cfg, "multi_alsa.use_mmap_if_available", &str))
        {
            if (strcasecmp(str, "no") == 0)
                config.no_mmap = 1;
            else if (strcasecmp(str, "yes") == 0)
                config.no_mmap = 0;
            else
            {
                warn("Invalid use_mmap_if_available option choice \"%s\". It should be "
                     "\"yes\" or \"no\". "
                     "It remains set to \"yes\".");
                config.no_mmap = 0;
            }
        }

        /* Get the optional period size value */
        if (config_lookup_int(config.cfg, "multi_alsa.period_size", &value))
        {
            unsigned int idx;
            for (idx = 0; idx < icountvalue; idx++)
                alsa_outputs_g.outputs[idx].set_period_size_request = 1;
            debug(1, "Value read for period size is %d.", value);
            if (value < 0)
            {
                warn("Invalid alsa period size setting \"%d\". It "
                     "must be greater than 0. No setting is made.",
                     value);
                for (idx = 0; idx < icountvalue; idx++)
                    alsa_outputs_g.outputs[idx].set_period_size_request = 0;
            }
            else
            {
                for (idx = 0; idx < icountvalue; idx++)
                    alsa_outputs_g.outputs[idx].period_size_requested = value;
            }
        }

        /* Get the optional buffer size value */
        if (config_lookup_int(config.cfg, "multi_alsa.buffer_size", &value))
        {
            unsigned int idx;
            for (idx = 0; idx < icountvalue; idx++)
                alsa_outputs_g.outputs[idx].set_buffer_size_request = 1;
            debug(1, "Value read for buffer size is %d.", value);
            if (value < 0)
            {
                warn("Invalid alsa buffer size setting \"%d\". It "
                     "must be greater than 0. No setting is made.",
                     value);
                for (idx = 0; idx < icountvalue; idx++)
                    alsa_outputs_g.outputs[idx].set_buffer_size_request = 0;
            }
            else
            {
                for (idx = 0; idx < icountvalue; idx++)
                    alsa_outputs_g.outputs[idx].buffer_size_requested = value;
            }
        }

        /* Get the optional alsa_maximum_stall_time setting. */
        if (config_lookup_float(config.cfg, "multi_alsa.maximum_stall_time", &dvalue))
        {
            if (dvalue < 0.0)
            {
                warn("Invalid alsa maximum write time setting \"%f\". It "
                     "must be greater than 0. Default is \"%f\". No setting is made.",
                     dvalue, config.alsa_maximum_stall_time);
            }
            else
            {
                config.alsa_maximum_stall_time = dvalue;
            }
        }

        /* Get the optional disable_standby_mode_silence_threshold setting. */
        if (config_lookup_float(config.cfg, "multi_alsa.disable_standby_mode_silence_threshold", &dvalue))
        {
            if (dvalue < 0.0)
            {
                warn("Invalid alsa disable_standby_mode_silence_threshold setting \"%f\". It "
                     "must be greater than 0. Default is \"%f\". No setting is made.",
                     dvalue, config.disable_standby_mode_silence_threshold);
            }
            else
            {
                config.disable_standby_mode_silence_threshold = dvalue;
            }
        }

        /* Get the optional disable_standby_mode_silence_scan_interval setting. */
        if (config_lookup_float(config.cfg, "multi_alsa.disable_standby_mode_silence_scan_interval",
                                &dvalue))
        {
            if (dvalue < 0.0)
            {
                warn("Invalid alsa disable_standby_mode_silence_scan_interval setting \"%f\". It "
                     "must be greater than 0. Default is \"%f\". No setting is made.",
                     dvalue, config.disable_standby_mode_silence_scan_interval);
            }
            else
            {
                config.disable_standby_mode_silence_scan_interval = dvalue;
            }
        }

        /* Get the optional disable_standby_mode setting. */
        if (config_lookup_string(config.cfg, "multi_alsa.disable_standby_mode", &str))
        {
            if ((strcasecmp(str, "no") == 0) || (strcasecmp(str, "off") == 0) ||
                (strcasecmp(str, "never") == 0))
                config.disable_standby_mode = disable_standby_off;
            else if ((strcasecmp(str, "yes") == 0) || (strcasecmp(str, "on") == 0) ||
                     (strcasecmp(str, "always") == 0))
            {
                config.disable_standby_mode = disable_standby_always;
                config.keep_dac_busy = 1;
            }
            else if (strcasecmp(str, "auto") == 0)
                config.disable_standby_mode = disable_standby_auto;
            else
            {
                warn("Invalid disable_standby_mode option choice \"%s\". It should be "
                     "\"always\", \"auto\" or \"never\". "
                     "It remains set to \"never\".",
                     str);
            }
        }

        if (config_lookup_string(config.cfg, "multi_alsa.use_precision_timing", &str))
        {
            if ((strcasecmp(str, "no") == 0) || (strcasecmp(str, "off") == 0) ||
                (strcasecmp(str, "never") == 0))
                config.use_precision_timing = YNA_NO;
            else if ((strcasecmp(str, "yes") == 0) || (strcasecmp(str, "on") == 0) ||
                     (strcasecmp(str, "always") == 0))
            {
                config.use_precision_timing = YNA_YES;
                config.keep_dac_busy = 1;
            }
            else if (strcasecmp(str, "auto") == 0)
                config.use_precision_timing = YNA_AUTO;
            else
            {
                warn("Invalid use_precision_timing option choice \"%s\". It should be "
                     "\"yes\", \"auto\" or \"no\". "
                     "It remains set to \"%s\".",
                     config.use_precision_timing == YNA_NO     ? "no"
                     : config.use_precision_timing == YNA_AUTO ? "auto"
                                                               : "yes");
            }
        }

        debug(1, "alsa: disable_standby_mode is \"%s\".",
              config.disable_standby_mode == disable_standby_off      ? "never"
              : config.disable_standby_mode == disable_standby_always ? "always"
                                                                      : "auto");
        debug(1, "alsa: disable_standby_mode_silence_threshold is %f seconds.",
              config.disable_standby_mode_silence_threshold);
        debug(1, "alsa: disable_standby_mode_silence_scan_interval is %f seconds.",
              config.disable_standby_mode_silence_scan_interval);
    }

    optind = 1; // optind=0 is equivalent to optind=1 plus special behaviour
    argv--;     // so we shift the arguments to satisfy getopt()
    argc++;
    // some platforms apparently require optreset = 1; - which?
    int opt;
    char *arg_devices;
    char *arg_mcontrols;
    char *arg_mdevices;
    char *arg_mindexes;
    char *arg_delimeter;
    arg_devices = NULL;
    arg_mcontrols = NULL;
    arg_mdevices = NULL;
    arg_mindexes = NULL;
    arg_delimeter = NULL;

    while ((opt = getopt(argc, argv, "d:t:m:c:i:x:")) > 0)
    {
        switch (opt)
        {
        case 'd':
            arg_devices = optarg;
            break;

        case 't':
            inform("The alsa backend -t option is deprecated and has been ignored. "
                   "FYI, using the -c option automatically chooses a hardware "
                   "mixer.");
            break;

        case 'm':
            arg_mdevices = optarg;
            break;
        case 'c':
            arg_mcontrols = optarg;
            break;
        case 'i':
            arg_mindexes = optarg;
            break;
        case 'x':
            arg_delimeter = optarg;
            break;
        default:
            warn("Invalid audio option \"-%c\" specified -- ignored.", opt);
            help();
        }
    }

    if ((arg_devices != NULL) && (strlen(arg_devices) > 0))
    {
        int idx;
        int devcount = 0;
        int startpoint = alsa_outputs_g.output_count;
        if (startpoint > 0)
        {
            startpoint -= 1;
        }

        char *a_device = NULL;
        if (arg_delimeter == 0)
        {
            arg_delimeter = " ";
        }

        while ((a_device = strsep(&arg_devices, arg_delimeter)))
        {
            if ((a_device != NULL) && (strlen(a_device) > 0))
            {
                alsa_output *a_new_output;
                if (alsa_outputs_g.outputs == NULL)
                {
                    alsa_outputs_g.outputs = (alsa_output *)malloc(1 * sizeof(alsa_output));
                    if (alsa_outputs_g.outputs == NULL)
                    {
                        die("audio_multi_alsa: init() : Failed to allocate memory of %d for alsa_outputs."
                            " The alsa_outputs pointer is NULL.",
                            1 * sizeof(alsa_output));
                    }

                    idx = 0;
                    memset(&alsa_outputs_g.outputs[idx], 0, sizeof(alsa_output));
                    initialize_alsa_output(&alsa_outputs_g.outputs[idx]);
                    alsa_outputs_g.outputs[idx].alsa_device_initialised = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_error_threshold = temp_stall_monitor_error_threshold;
                    alsa_outputs_g.outputs[idx].stall_monitor_start_time = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_frame_count = 0;
                    alsa_outputs_g.outputs[idx].alsa_backend_state = abm_disconnected; // startup state
                    alsa_outputs_g.outputs[idx].delay_and_status = standard_delay_and_status;
                    a_new_output = &alsa_outputs_g.outputs[idx];
                    alsa_outputs_g.output_count = 1;
                }
                else
                {
                    alsa_outputs_g.outputs = (alsa_output *)realloc(alsa_outputs_g.outputs, (alsa_outputs_g.output_count + 1) * sizeof(alsa_output));
                    if (alsa_outputs_g.outputs == NULL)
                    {
                        die("audio_multi_alsa: init() : Failed to allocate memory of %d for alsa_outputs."
                            " The alsa_outputs pointer is NULL.",
                            1 * sizeof(alsa_output));
                    }

                    idx = alsa_outputs_g.output_count;
                    memset(&alsa_outputs_g.outputs[idx], 0, sizeof(alsa_output));
                    initialize_alsa_output(&alsa_outputs_g.outputs[idx]);
                    alsa_outputs_g.outputs[idx].alsa_device_initialised = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_error_threshold = temp_stall_monitor_error_threshold;
                    alsa_outputs_g.outputs[idx].stall_monitor_start_time = 0;
                    alsa_outputs_g.outputs[idx].stall_monitor_frame_count = 0;
                    alsa_outputs_g.outputs[idx].alsa_backend_state = abm_disconnected; // startup state
                    alsa_outputs_g.outputs[idx].delay_and_status = standard_delay_and_status;
                    a_new_output = &alsa_outputs_g.outputs[idx];
                    alsa_outputs_g.output_count = alsa_outputs_g.output_count + 1;
                }

                a_new_output->stall_monitor_error_threshold = temp_stall_monitor_error_threshold;
                a_new_output->alsa_backend_state = abm_disconnected; // startup state
                a_new_output->delay_and_status = standard_delay_and_status;
                a_new_output->alsa_characteristics_already_listed = 0;

                a_new_output->alsa_out_dev = a_device;
                debug(2, "multi_alsa: output device [%d] name is \"%s\".", (devcount + 1), a_new_output->alsa_out_dev);
                devcount++;
            }
        }

        if ((arg_mcontrols != NULL) && (strlen(arg_mcontrols) > 0))
        {
            int itt = startpoint;
            while ((a_device = strsep(&arg_mcontrols, arg_delimeter)))
            {
                if ((a_device != NULL) && (strlen(a_device) > 0))
                {
                    alsa_output *a_new_output = &alsa_outputs_g.outputs[itt];
                    a_new_output->alsa_mix_ctrl = a_device;
                    itt++;
                }
            }
        }

        if ((arg_mdevices != NULL) && (strlen(arg_mdevices) > 0))
        {
            int itt = startpoint;
            while ((a_device = strsep(&arg_mdevices, arg_delimeter)))
            {
                if ((a_device != NULL) && (strlen(a_device) > 0))
                {
                    alsa_output *a_new_output = &alsa_outputs_g.outputs[itt];
                    a_new_output->alsa_mix_dev = a_device;
                    itt++;
                }
            }
        }

        if ((arg_mindexes != NULL) && (strlen(arg_mindexes) > 0))
        {
            int itt = startpoint;
            while ((a_device = strsep(&arg_mindexes, arg_delimeter)))
            {
                if ((a_device != NULL) && (strlen(a_device) > 0))
                {
                    alsa_output *a_new_output = &alsa_outputs_g.outputs[itt];
                    a_new_output->alsa_mix_index = strtol(a_device, NULL, 10);
                    ;
                    itt++;
                }
            }
        }
    }

    if (optind < argc)
    {
        warn("Invalid audio argument: \"%s\" -- ignored", argv[optind]);
    }

    // so, now, if the option to keep the DAC running has been selected, start a
    // thread to monitor the
    // length of the queue
    // if the queue gets too short, stuff it with silence
    {
        int idx;
        for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
        {
            pthread_create(&alsa_outputs_g.outputs[idx].alsa_buffer_monitor_thread, NULL, &alsa_buffer_monitor_thread_code, &alsa_outputs_g.outputs[idx]);
        }
    }

    return response;
}

static void deinit(void)
{
    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
    // debug(2,"audio_alsa deinit called.");
    stop();
    debug(2, "Cancel buffer monitor threads.");
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        debug(2, "Canceling buffer monitor threads. %d", idx);
        pthread_cancel(alsa_outputs_g.outputs[idx].alsa_buffer_monitor_thread);
    }

    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        debug(3, "Joining buffer monitor thread. %d", idx);
        pthread_join(alsa_outputs_g.outputs[idx].alsa_buffer_monitor_thread, NULL);
    }

    pthread_setcancelstate(oldState, NULL);
}

static int set_mute_state(alsa_output *output)
{
    int response = 1; // some problem expected, e.g. no mixer or not allowed to use it or disconnected
    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
    pthread_cleanup_debug_mutex_lock(&output->alsa_mixer_mutex, 10000, 0);

    if ((output->alsa_backend_state != abm_disconnected) && (config.alsa_use_hardware_mute == 1) && (open_mixer(output) == 0))
    {
        response = 0; // okay if actually using the mute facility
        debug(2, "multi_alsa: actually set_mute_state");
        int mute = 0;
        if ((output->mute_requested_externally != 0) || (output->mute_requested_internally != 0))
        {
            mute = 1;
        }
        if (mute == 1)
        {
            debug(2, "multi_alsa: hardware mute switched on");
            if (snd_mixer_selem_has_playback_switch(output->alsa_mix_elem))
            {
                snd_mixer_selem_set_playback_switch_all(output->alsa_mix_elem, 0);
            }
            else
            {
                output->volume_based_mute_is_active = 1;
                do_snd_mixer_selem_set_playback_dB_all(output->alsa_mix_elem, output->alsa_mix_mute);
            }
        }
        else
        {
            debug(2, "multi_alsa: hardware mute switched off");
            if (snd_mixer_selem_has_playback_switch(output->alsa_mix_elem))
            {
                snd_mixer_selem_set_playback_switch_all(output->alsa_mix_elem, 1);
            }
            else
            {
                output->volume_based_mute_is_active = 0;
                do_snd_mixer_selem_set_playback_dB_all(output->alsa_mix_elem, output->set_volume);
            }
        }

        close_mixer(output);
    }

    debug_mutex_unlock(&output->alsa_mixer_mutex, 3); // release the mutex
    pthread_cleanup_pop(0);                           // release the mutex
    pthread_setcancelstate(oldState, NULL);
    return response;
}

static void start(__attribute__((unused)) int i_sample_rate,
                  __attribute__((unused)) int i_sample_format)
{
    debug(3, "audio_multi_alsa start called.");
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        // frame_index = 0;
        // measurement_data_is_valid = 0;

        alsa_outputs_g.outputs[idx].stall_monitor_start_time = 0;
        alsa_outputs_g.outputs[idx].stall_monitor_frame_count = 0;
        if (alsa_outputs_g.outputs[idx].alsa_device_initialised == 0)
        {
            debug(1, "multi_alsa: start() calling alsa_device_init. \"%s\"", alsa_outputs_g.outputs[idx].alsa_out_dev);
            alsa_device_init(&alsa_outputs_g.outputs[idx]);
            alsa_outputs_g.outputs[idx].alsa_device_initialised = 1;
        }
    }
}

static int standard_delay_and_status(alsa_output *output, snd_pcm_state_t *state, snd_pcm_sframes_t *delay,
                                     yndk_type *using_update_timestamps)
{
    int ret = output->alsa_handle_status;
    if (using_update_timestamps)
    {
        *using_update_timestamps = YNDK_NO;
    }

    snd_pcm_state_t state_temp = SND_PCM_STATE_DISCONNECTED;
    snd_pcm_sframes_t delay_temp = 0;
    if (output->alsa_handle != NULL)
    {
        state_temp = snd_pcm_state(output->alsa_handle);
        if ((state_temp == SND_PCM_STATE_RUNNING) || (state_temp == SND_PCM_STATE_DRAINING))
        {
            ret = snd_pcm_delay(output->alsa_handle, &delay_temp);
        }
        else
        {
            // not running, thus no delay information, thus can't check for frame
            // rates
            // frame_index = 0; // we'll be starting over...
            // measurement_data_is_valid = 0;
            // delay_temp = 0;
            ret = 0;
        }
    }
    // else
    //{
    // debug(1, "alsa_handle is NULL in standard_delay_and_status.");
    //}

    output->stall_monitor_start_time = 0;  // zero if not initialised / not started / zeroed by flush
    output->stall_monitor_frame_count = 0; // set to delay at start of time, incremented by any writes

    if (delay != NULL)
    {
        *delay = delay_temp;
    }
    if (state != NULL)
    {
        *state = state_temp;
    }

    return ret;
}

static int precision_delay_and_status(alsa_output *output, snd_pcm_state_t *state, snd_pcm_sframes_t *delay,
                                      yndk_type *using_update_timestamps)
{
    snd_pcm_state_t state_temp = SND_PCM_STATE_DISCONNECTED;
    snd_pcm_sframes_t delay_temp = 0;

    if (using_update_timestamps)
    {
        *using_update_timestamps = YNDK_DONT_KNOW;
    }

    int ret = output->alsa_handle_status;

    snd_pcm_status_t *alsa_snd_pcm_status;
    snd_pcm_status_alloca(&alsa_snd_pcm_status);

    struct timespec tn;                // time now
    snd_htimestamp_t update_timestamp; // actually a struct timespec
    if (output->alsa_handle != NULL)
    {
        ret = snd_pcm_status(output->alsa_handle, alsa_snd_pcm_status);
        if (ret == 0)
        {
            snd_pcm_status_get_htstamp(alsa_snd_pcm_status, &update_timestamp);

            /*
            // must be 1.1 or later to use snd_pcm_status_get_driver_htstamp
            #if SND_LIB_MINOR != 0
                snd_htimestamp_t driver_htstamp;
                snd_pcm_status_get_driver_htstamp(alsa_snd_pcm_status, &driver_htstamp);
                uint64_t driver_htstamp_ns = driver_htstamp.tv_sec;
                driver_htstamp_ns = driver_htstamp_ns * 1000000000;
                driver_htstamp_ns = driver_htstamp_ns + driver_htstamp.tv_nsec;
                debug(1,"driver_htstamp: %f.", driver_htstamp_ns * 0.000000001);
            #endif
            */

            state_temp = snd_pcm_status_get_state(alsa_snd_pcm_status);

            if ((state_temp == SND_PCM_STATE_RUNNING) || (state_temp == SND_PCM_STATE_DRAINING))
            {

                //     uint64_t update_timestamp_ns =
                //         update_timestamp.tv_sec * (uint64_t)1000000000 + update_timestamp.tv_nsec;

                uint64_t update_timestamp_ns = update_timestamp.tv_sec;
                update_timestamp_ns = update_timestamp_ns * 1000000000;
                update_timestamp_ns = update_timestamp_ns + update_timestamp.tv_nsec;

                // if the update_timestamp is zero, we take this to mean that the device doesn't report
                // interrupt timings. (It could be that it's not a real hardware device.)
                // so we switch to getting the delay the regular way
                // i.e. using snd_pcm_delay	()
                if (using_update_timestamps)
                {
                    if (update_timestamp_ns == 0)
                    {
                        *using_update_timestamps = YNDK_NO;
                    }
                    else
                    {
                        *using_update_timestamps = YNDK_YES;
                    }
                }

                // user information
                if (update_timestamp_ns == 0)
                {
                    if (output->delay_type_notified != 1)
                    {
                        debug(2, "multi_alsa: update timestamps unavailable");
                        output->delay_type_notified = 1;
                    }
                }
                else
                {
                    // diagnostic
                    if (output->delay_type_notified != 0)
                    {
                        debug(2, "multi_alsa: update timestamps available");
                        output->delay_type_notified = 0;
                    }
                }

                if (update_timestamp_ns == 0)
                {
                    ret = snd_pcm_delay(output->alsa_handle, &delay_temp);
                }
                else
                {
                    delay_temp = snd_pcm_status_get_delay(alsa_snd_pcm_status);

                    /*
                    // It seems that the alsa library uses CLOCK_REALTIME before 1.0.28, even though
                    // the check for monotonic returns true. Might have to watch out for this.
                    #if SND_LIB_MINOR == 0 && SND_LIB_SUBMINOR < 28
                            clock_gettime(CLOCK_REALTIME, &tn);
                    #else
                            clock_gettime(CLOCK_MONOTONIC, &tn);
                    #endif
                    */

                    if (output->use_monotonic_clock)
                    {
                        clock_gettime(CLOCK_MONOTONIC, &tn);
                    }
                    else
                    {
                        clock_gettime(CLOCK_REALTIME, &tn);
                    }

                    // uint64_t time_now_ns = tn.tv_sec * (uint64_t)1000000000 + tn.tv_nsec;
                    uint64_t time_now_ns = tn.tv_sec;
                    time_now_ns = time_now_ns * 1000000000;
                    time_now_ns = time_now_ns + tn.tv_nsec;

                    // see if it's stalled

                    if ((output->stall_monitor_start_time != 0) && (output->stall_monitor_frame_count == delay_temp))
                    {
                        // hasn't outputted anything since the last call to delay()

                        if (((update_timestamp_ns - output->stall_monitor_start_time) >
                             output->stall_monitor_error_threshold) ||
                            ((time_now_ns - output->stall_monitor_start_time) > output->stall_monitor_error_threshold))
                        {
                            debug(2,
                                  "DAC seems to have stalled with time_now_ns: %" PRIX64
                                  ", update_timestamp_ns: %" PRIX64 ", stall_monitor_start_time %" PRIX64
                                  ", stall_monitor_error_threshold %" PRIX64 ".",
                                  time_now_ns, update_timestamp_ns, output->stall_monitor_start_time,
                                  output->stall_monitor_error_threshold);
                            debug(2,
                                  "DAC seems to have stalled with time_now: %lx,%lx"
                                  ", update_timestamp: %lx,%lx, stall_monitor_start_time %" PRIX64
                                  ", stall_monitor_error_threshold %" PRIX64 ".",
                                  tn.tv_sec, tn.tv_nsec, update_timestamp.tv_sec, update_timestamp.tv_nsec,
                                  output->stall_monitor_start_time, output->stall_monitor_error_threshold);
                            ret = sps_extra_code_output_stalled;
                        }
                    }
                    else
                    {
                        output->stall_monitor_start_time = update_timestamp_ns;
                        output->stall_monitor_frame_count = delay_temp;
                    }

                    if (ret == 0)
                    {
                        uint64_t delta = time_now_ns - update_timestamp_ns;

                        //          uint64_t frames_played_since_last_interrupt =
                        //              ((uint64_t)config.output_rate * delta) / 1000000000;

                        uint64_t frames_played_since_last_interrupt = config.output_rate;
                        frames_played_since_last_interrupt = frames_played_since_last_interrupt * delta;
                        frames_played_since_last_interrupt = frames_played_since_last_interrupt / 1000000000;

                        snd_pcm_sframes_t frames_played_since_last_interrupt_sized = frames_played_since_last_interrupt;
                        if ((frames_played_since_last_interrupt_sized < 0) ||
                            ((uint64_t)frames_played_since_last_interrupt_sized !=
                             frames_played_since_last_interrupt))
                            debug(1,
                                  "overflow resizing frames_played_since_last_interrupt %" PRIx64
                                  " to frames_played_since_last_interrupt %lx.",
                                  frames_played_since_last_interrupt, frames_played_since_last_interrupt_sized);
                        delay_temp = delay_temp - frames_played_since_last_interrupt_sized;
                    }
                }
            }
            else
            {
                // not running, thus no delay information, thus can't check for
                // stall
                delay_temp = 0;
                output->stall_monitor_start_time = 0;  // zero if not initialised / not started / zeroed by flush
                output->stall_monitor_frame_count = 0; // set to delay at start of time, incremented by any writes

                // not running, thus no delay information, thus can't check for frame
                // rates
                // frame_index = 0; // we'll be starting over...
                // measurement_data_is_valid = 0;
            }
        }
        else
        {
            debug(1, "multi_alsa: can't get device's status.");
        }
    }
    else
    {
        debug(2, "multi_alsa_handle is NULL in precision_delay_and_status!");
    }
    if (delay != NULL)
    {
        *delay = delay_temp;
    }
    if (state != NULL)
    {
        *state = state_temp;
    }

    return ret;
}

static int delay(long *the_delay)
{
    // returns 0 if the device is in a valid state -- SND_PCM_STATE_RUNNING or
    // SND_PCM_STATE_PREPARED
    // or SND_PCM_STATE_DRAINING
    // and returns the actual delay if running or 0 if prepared in *the_delay

    // otherwise return an error code
    // the error code could be a Unix errno code or a snderror code, or
    // the sps_extra_code_output_stalled or the
    // sps_extra_code_output_state_cannot_make_ready codes
    int ret = 0;
    snd_pcm_sframes_t my_delay = 0;

    int oldState;

    snd_pcm_state_t state;

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable

    int idx;
    if (alsa_outputs_g.output_count > 0)
    {
        for (idx = alsa_outputs_g.output_count - 1; idx >= 0; idx--)
        {
            pthread_cleanup_debug_mutex_lock(&alsa_outputs_g.outputs[idx].alsa_mutex, 10000, 0);
            ret = alsa_outputs_g.outputs[idx].delay_and_status(&alsa_outputs_g.outputs[idx], &state, &my_delay, NULL);
            debug_mutex_unlock(&alsa_outputs_g.outputs[idx].alsa_mutex, 0);
            pthread_cleanup_pop(0);
            break;
        }
    }

    pthread_setcancelstate(oldState, NULL);

    if (the_delay != NULL) // can't imagine why this might happen
    {
        *the_delay = my_delay; // note: snd_pcm_sframes_t is a long
    }

    return ret;
}

static int stats(uint64_t *raw_measurement_time, uint64_t *corrected_measurement_time,
                 uint64_t *the_delay, uint64_t *frames_sent_to_dac)
{
    // returns 0 if the device is in a valid state -- SND_PCM_STATE_RUNNING or
    // SND_PCM_STATE_PREPARED
    // or SND_PCM_STATE_DRAINING.
    // returns the actual delay if running or 0 if prepared in *the_delay
    // returns the present value of frames_sent_for_playing
    // otherwise return a non-zero value
    int ret = 0;
    *the_delay = 0;

    int oldState;

    snd_pcm_state_t state;
    snd_pcm_sframes_t my_delay = 0; // this initialisation is to silence a clang warning

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable

    if (alsa_outputs_g.output_count > 0)
    {
        int idx;
        for (idx = alsa_outputs_g.output_count - 1; idx >= 0; idx--)
        {
            alsa_output *output = &alsa_outputs_g.outputs[idx];
            pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 10000, 0);
            if (output->alsa_handle == NULL)
            {
                ret = output->alsa_handle_status;
            }
            else
            {
                *raw_measurement_time = get_absolute_time_in_ns();        // this is not conditioned ("disciplined") by NTP
                *corrected_measurement_time = get_monotonic_time_in_ns(); // this is ("disciplined") by NTP
                ret = output->delay_and_status(output, &state, &my_delay, NULL);
            }

            if (ret == 0)
            {
                ret = output->frames_sent_break_occurred; // will be zero unless an error like an underrun occurred
            }
            else
            {
                ret = 1; // just indicate there was some kind of a break
            }
            output->frames_sent_break_occurred = 0; // reset it.
            if (frames_sent_to_dac != NULL)
            {
                *frames_sent_to_dac = output->frames_sent_for_playing;
            }

            debug_mutex_unlock(&output->alsa_mutex, 0);
            pthread_cleanup_pop(0);
        }
    }

    pthread_setcancelstate(oldState, NULL);
    uint64_t hd = my_delay; // note: snd_pcm_sframes_t is a long
    *the_delay = hd;
    return ret;
}

static int do_play(alsa_output *output, void *buf, int samples)
{
    // assuming the alsa_mutex has been acquired
    int response = 0;
    if ((samples != 0) && (buf != NULL))
    {
        int oldState;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
        snd_pcm_state_t state;
        snd_pcm_sframes_t my_delay;

        response = output->delay_and_status(output, &state, &my_delay, NULL);

        if (response == 0) // will be non-zero if an error or a stall
        {
            // just check the state of the DAC
            if ((state != SND_PCM_STATE_PREPARED) && (state != SND_PCM_STATE_RUNNING) && (state != SND_PCM_STATE_XRUN))
            {
                debug(1, "multi_alsa: DAC %s in odd SND_PCM_STATE_* %d prior to writing.", output->alsa_out_dev, state);
            }

            snd_pcm_state_t prior_state = state; // keep this for afterwards....
            // debug(3, "multi_alsa: write %d frames.", samples);
            response = output->alsa_pcm_write(output->alsa_handle, buf, samples);
            if (response > 0)
            {
                output->frames_sent_for_playing += response; // this is the number of frames accepted
            }

            if (response == samples)
            {
                output->stall_monitor_frame_count += samples;
            }
            else
            {
                output->frames_sent_break_occurred = 1; // note than an output error has occurred
                if (response == -EPIPE)                 /* underrun */
                {
                    // It could be that the DAC was in the SND_PCM_STATE_XRUN state before
                    // sending the samples to be output. If so, it will still be in
                    // the SND_PCM_STATE_XRUN state after the call and it needs to be recovered.

                    // The underrun occurred in the past, so flagging an
                    // error at this point is misleading.

                    // In fact, having put samples in the buffer, we are about to fix it by now
                    // issuing a snd_pcm_recover().

                    // So, if state is SND_PCM_STATE_XRUN now, only report it if the state was
                    // not SND_PCM_STATE_XRUN prior to the call, i.e. report it only
                    // if we are not trying to recover from a previous underrun.
                    if (prior_state == SND_PCM_STATE_XRUN)
                    {
                        debug(1, "multi_alsa: %s recovering from a previous underrun.", output->alsa_out_dev);
                    }
                    else
                    {
                        debug(1, "multi_alsa: underrun while writing %d samples to alsa device %s.", samples, output->alsa_out_dev);
                    }

                    response = snd_pcm_recover(output->alsa_handle, response, 1);
                }
                else if (response == -ESTRPIPE) /* suspended */
                {
                    if (state != prior_state)
                    {
                        debug(1, "multi_alsa: suspended while writing %d samples to alsa device %s.", samples, output->alsa_out_dev);
                    }

                    if ((response = snd_pcm_resume(output->alsa_handle)) == -ENOSYS)
                    {
                        response = snd_pcm_prepare(output->alsa_handle);
                    }
                }
                else if (response >= 0)
                {
                    debug(1, "multi_alsa: only %d of %d samples output.", response, samples);
                }
            }
        }

        pthread_setcancelstate(oldState, NULL);
        if (response < 0)
        {
            char errorstring[1024];
            strerror_r(-response, (char *)errorstring, sizeof(errorstring));
            debug(1, "multi_alsa: SND_PCM_STATE_* %d, error %d (\"%s\") writing %d samples to alsa device.",
                  state, response, (char *)errorstring, samples);
        }

        if ((response == -ENOENT) || (response == -ENODEV)) // if the device isn't there...
        {
            handle_unfixable_error(-response);
        }
    }

    return response;
}

static int do_open(alsa_output *output, int do_auto_setup)
{
    int response = 0;
    if (output->alsa_backend_state != abm_disconnected)
    {
        debug(1, "multi_alsa: do_open() -- opening the output device %s when it is already connected", output->alsa_out_dev);
    }

    if (output->alsa_handle == NULL)
    {
        // debug(1,"multi_alsa: do_open() -- opening the output device");
        response = open_alsa_device(output, do_auto_setup);
        if (response == 0)
        {
            output->mute_requested_internally = 0;
            if (audio_multi_alsa.volume)
            {
                do_volume(output, output->set_volume);
            }

            if (audio_multi_alsa.mute)
            {
                debug(2, "do_open() set_mute_state");
                set_mute_state(output); // the mute_requested_externally flag will have been
                                        // set accordingly
                // do_mute(output, 0);          // complete unmute
            }

            output->frames_sent_break_occurred = 1; // there is a discontinuity with
            // any previously-reported frame count

            output->frames_sent_for_playing = 0;
            output->alsa_backend_state = abm_connected; // only do this if it really opened it.
        }
        else
        {
            if ((response == -ENOENT) || (response == -ENODEV)) // if the device isn't there...
            {
                handle_unfixable_error(-response);
            }
        }
    }
    else
    {
        debug(1, "multi_alsa: do_open() -- output device %s already open.", output->alsa_out_dev);
    }

    return response;
}

static int do_close(alsa_output *output)
{
    debug(2, "multi_alsa: do_close()");
    if (output->alsa_backend_state == abm_disconnected)
    {
        debug(1, "multi_alsa: do_close() -- closing the output device when it is already disconnected");
    }

    int derr = 0;
    if (output->alsa_handle)
    {
        // debug(1,"multi_alsa: do_close() -- closing the output device");
        if ((derr = snd_pcm_drop(output->alsa_handle)))
        {
            debug(1, "Error %d (\"%s\") dropping output device.", derr, snd_strerror(derr));
        }
        usleep(10000); // wait for the hardware to do its trick. BTW, this make the function pthread
                       // cancellable
        if ((derr = snd_pcm_hw_free(output->alsa_handle)))
        {
            debug(1, "Error %d (\"%s\") freeing the output device hardware.", derr, snd_strerror(derr));
        }

        debug(2, "multi_alsa: do_close() -- closing alsa handle");
        if ((derr = snd_pcm_close(output->alsa_handle)))
        {
            debug(1, "Error %d (\"%s\") closing the output device.", derr, snd_strerror(derr));
        }

        output->alsa_handle = NULL;
        output->alsa_handle_status = ENODEV; // no device open
    }
    else
    {
        debug(1, "multi_alsa: do_close() -- output device already closed.");
    }

    output->alsa_backend_state = abm_disconnected;

    return derr;
}

static int sub_flush(alsa_output *output)
{
    if (output->alsa_backend_state == abm_disconnected)
    {
        debug(1, "multi_alsa: do_flush() -- closing the output device when it is already disconnected");
    }

    int derr = 0;
    if (output->alsa_handle)
    {
        debug(2, "multi_alsa: do_flush() -- flushing the output device");
        output->frames_sent_break_occurred = 1;
        if ((derr = snd_pcm_drop(output->alsa_handle)))
        {
            debug(1, "Error %d (\"%s\") dropping output device.", derr, snd_strerror(derr));
        }
        if ((derr = snd_pcm_prepare(output->alsa_handle)))
        {
            debug(1, "Error %d (\"%s\") preparing output device after flush.", derr, snd_strerror(derr));
        }
        output->alsa_backend_state = abm_connected;
    }
    else
    {
        debug(1, "multi_alsa: do_flush() -- output device already closed.");
    }

    return derr;
}

static int play(void *buf, int samples, __attribute__((unused)) int sample_type,
                __attribute__((unused)) uint32_t timestamp,
                __attribute__((unused)) uint64_t playtime)
{
    // play() will change the state of the alsa_backend_mode to abm_playing
    // also, if the present alsa_backend_state is abm_disconnected, then first the
    // DAC must be
    // connected

    // debug(3,"audio_alsa play called.");
    int response = 0;

    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        response = 0;
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 50000, 0);
        if (output->alsa_backend_state == abm_disconnected)
        {
            response = do_open(&alsa_outputs_g.outputs[idx], 0);
            if (response == 0)
            {
                debug(2, "multi_alsa: play() -- opened output device");
            }
        }

        if (response == 0)
        {
            if (output->alsa_backend_state != abm_playing)
            {
                debug(2, "multi_alsa: play() -- alsa_backend_state => abm_playing");
                // mute_requested_internally = 0; // stop requesting a mute for backend's own
                // reasons, which might have been a flush
                // debug(2, "play() set_mute_state");
                // set_mute_state(); // try to action the request and return a status
                // do_mute(0); // unmute for backend's reason
            }

            output->alsa_backend_state = abm_playing;
            response = do_play(output, buf, samples);
        }

        debug_mutex_unlock(&output->alsa_mutex, 0);
        pthread_cleanup_pop(0); // release the mutex
    }

    return response;
}

static int prepare(void)
{
    // this will leave the DAC open / connected.
    int ret = 0;
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 50000, 0);
        if (output->alsa_backend_state == abm_disconnected)
        {
            if (output->alsa_device_initialised == 0)
            {
                // debug(1, "multi_alsa: prepare() calling alsa_device_init.");
                alsa_device_init(output);
                output->alsa_device_initialised = 1;
            }
            ret = do_open(output, 1); // do auto setup
            if (ret == 0)
            {
                debug(2, "multi_alsa: prepare() -- opened output device");
            }
        }

        debug_mutex_unlock(&output->alsa_mutex, 0);
        pthread_cleanup_pop(0); // release the mutex
    }

    return ret;
}

static void flush(void)
{
    // debug(2,"audio_alsa flush called.");
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 10000, 1);
        if (output->alsa_backend_state != abm_disconnected) // must be playing or connected...
        {
            // do nothing for a flush if config.keep_dac_busy is true
            if (config.keep_dac_busy == 0)
            {
                sub_flush(output);
            }
        }
        else
        {
            debug(3, "multi_alsa: flush() -- called on a disconnected alsa backend");
        }

        debug_mutex_unlock(&output->alsa_mutex, 3);
        pthread_cleanup_pop(0); // release the mutex
    }
}

static void stop(void)
{
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 10000, 1);
        if (output->alsa_backend_state != abm_disconnected) // must be playing or connected...
        {
            if (config.keep_dac_busy == 0)
            {
                do_close(output);
            }
        }
        else
        {
            debug(3, "multi_alsa: stop() -- called on a disconnected alsa backend");
        }
        debug_mutex_unlock(&output->alsa_mutex, 3);
        pthread_cleanup_pop(0); // release the mutex
    }
}

static void parameters(audio_parameters *info)
{
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        info->minimum_volume_dB = output->alsa_mix_mindb;
        info->maximum_volume_dB = output->alsa_mix_maxdb;
        break;
    }
}

static void do_volume(alsa_output *output, double vol) // caller is assumed to have the alsa_mutex when
                                                       // using this function
{
    debug(3, "Setting volume db to %f.", vol);
    int oldState;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // make this un-cancellable
    output->set_volume = vol;
    pthread_cleanup_debug_mutex_lock(&output->alsa_mixer_mutex, 1000, 1);
    if (output->volume_set_request && (open_mixer(output) == 0))
    {
        if (output->has_softvol)
        {
            if (output->ctl && output->elem_id)
            {
                snd_ctl_elem_value_t *value;
                long raw;

                if (snd_ctl_convert_from_dB(output->ctl, output->elem_id, vol, &raw, 0) < 0)
                {
                    debug(1, "Failed converting dB gain to raw volume value for the software volume control.");
                }

                snd_ctl_elem_value_alloca(&value);
                snd_ctl_elem_value_set_id(value, output->elem_id);
                snd_ctl_elem_value_set_integer(value, 0, raw);
                snd_ctl_elem_value_set_integer(value, 1, raw);
                if (snd_ctl_elem_write(output->ctl, value) < 0)
                    debug(1, "Failed to set playback dB volume for the software volume control.");
            }
        }
        else
        {
            if (output->volume_based_mute_is_active == 0)
            {
                // debug(1,"Set alsa volume.");
                do_snd_mixer_selem_set_playback_dB_all(output->alsa_mix_elem, vol);
            }
            else
            {
                debug(2, "Not setting volume because volume-based mute is active");
            }
        }

        output->volume_set_request = 0; // any external request that has been made is now satisfied
        close_mixer(output);
    }

    debug_mutex_unlock(&output->alsa_mixer_mutex, 3);
    pthread_cleanup_pop(0); // release the mutex
    pthread_setcancelstate(oldState, NULL);
}

static void volume(double vol)
{
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        output->volume_set_request = 1; // an external request has been made to set the volume
        do_volume(output, vol);
    }
}

static int mute(int mute_state_requested) // these would be for external reasons, not
                                          // because of the
                                          // state of the backend.
{
    int response = 0;
    int idx;
    for (idx = 0; idx < alsa_outputs_g.output_count; idx++)
    {
        alsa_output *output = &alsa_outputs_g.outputs[idx];
        output->mute_requested_externally = mute_state_requested; // request a mute for external reasons
        debug(2, "mute(%d) set_mute_state", mute_state_requested);
        response = set_mute_state(output);
    }

    return response;
}

static void *alsa_buffer_monitor_thread_code(void *arg)
{
    alsa_output *output = (alsa_output *)arg;
    int frame_count = 0;
    int error_count = 0;
    int error_detected = 0;
    int okb = -1;
    // if too many play errors occur early on, we will turn off the disable standby mode
    while (error_detected == 0)
    {
        int keep_dac_busy_has_just_gone_off = 0;
        if (okb != config.keep_dac_busy)
        {
            if ((okb != 0) && (config.keep_dac_busy == 0))
            {
                keep_dac_busy_has_just_gone_off = 1;
            }

            debug(2, "keep_dac_busy is now \"%s\"", config.keep_dac_busy == 0 ? "no" : "yes");
            okb = config.keep_dac_busy;
        }

        if ((config.keep_dac_busy != 0) && (output->alsa_device_initialised == 0))
        {
            debug(2, "multi_alsa: alsa_buffer_monitor_thread_code() calling alsa_device_init.");
            alsa_device_init(output);
            output->alsa_device_initialised = 1;
        }

        int sleep_time_us = (int)(config.disable_standby_mode_silence_scan_interval * 1000000);
        pthread_cleanup_debug_mutex_lock(&output->alsa_mutex, 200000, 0);
        // check possible state transitions here
        if ((output->alsa_backend_state == abm_disconnected) && (config.keep_dac_busy != 0))
        {
            // open the dac and move to abm_connected mode
            if (do_open(output, 1) == 0) // no automatic setup of rate and speed if necessary
            {
                debug(2, "multi_alsa: alsa_buffer_monitor_thread_code() -- output device opened; alsa_backend_state => abm_connected");
            }
        }
        else if ((output->alsa_backend_state != abm_disconnected) && (keep_dac_busy_has_just_gone_off != 0))
        {
            output->stall_monitor_start_time = 0;
            // frame_index = 0;
            // measurement_data_is_valid = 0;
            debug(2, "multi_alsa: alsa_buffer_monitor_thread_code() -- closing the output device");
            do_close(output);
            debug(2, "multi_alsa: alsa_buffer_monitor_thread_code() -- alsa_backend_state => abm_disconnected");
        }
        // now, if the backend is not in the abm_disconnected state
        // and config.keep_dac_busy is true (at the present, this has to be the case
        // to be in the
        // abm_connected state in the first place...) then do the silence-filling
        // thing, if needed /* only if the output device is capable of precision delay */.
        if ((output->alsa_backend_state != abm_disconnected) && (config.keep_dac_busy != 0) /* && precision_delay_available(output) */)
        {
            int reply;
            long buffer_size = 0;
            snd_pcm_state_t state;
            reply = output->delay_and_status(output, &state, &buffer_size, NULL);
            if (reply != 0)
            {
                buffer_size = 0;
                char errorstring[1024];
                strerror_r(-reply, (char *)errorstring, sizeof(errorstring));
                debug(1, "multi_alsa: alsa_buffer_monitor_thread_code delay error %d: \"%s\".", reply, (char *)errorstring);
            }
            long buffer_size_threshold = (long)(config.disable_standby_mode_silence_threshold * config.output_rate);
            size_t size_of_silence_buffer;
            if (buffer_size < buffer_size_threshold)
            {
                int frames_of_silence = 1024;
                size_of_silence_buffer = frames_of_silence * output->frame_size;
                void *silence = malloc(size_of_silence_buffer);
                if (silence == NULL)
                {
                    warn("disable_standby_mode has been turned off because a memory allocation error occurred.");
                    error_detected = 1;
                }
                else
                {
                    int ret;
                    pthread_cleanup_push(malloc_cleanup, silence);
                    int use_dither = 0;
                    if ((output->alsa_mix_ctrl == NULL) && (config.ignore_volume_control == 0) && (config.airplay_volume != 0.0))
                    {
                        use_dither = 1;
                    }

                    output->dither_random_number_store =
                        generate_zero_frames(silence, frames_of_silence, config.output_format,
                                             use_dither, // i.e. with dither
                                             output->dither_random_number_store);
                    ret = do_play(output, silence, frames_of_silence);
                    frame_count++;
                    pthread_cleanup_pop(1); // free malloced buffer
                    if (ret < 0)
                    {
                        error_count++;
                        char errorstring[1024];
                        strerror_r(-ret, (char *)errorstring, sizeof(errorstring));
                        debug(2,
                              "multi_alsa: alsa_buffer_monitor_thread_code error %d (\"%s\") writing %d samples "
                              "to alsa device -- %d errors in %d trials.",
                              ret, (char *)errorstring, frames_of_silence, error_count, frame_count);
                        if ((error_count > 40) && (frame_count < 100))
                        {
                            warn("disable_standby_mode has been turned off because too many underruns "
                                 "occurred. Is Shairport Sync outputting to a virtual device or running in a "
                                 "virtual machine?");
                            error_detected = 1;
                        }
                    }
                }
            }
        }

        debug_mutex_unlock(&output->alsa_mutex, 0);
        pthread_cleanup_pop(0); // release the mutex
        usleep(sleep_time_us);  // has a cancellation point in it
    }

    pthread_exit(NULL);
}