/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file contains functions interacting with the CoreAudio framework
 * that are not specific to the AUHAL. These are split in a separate file for
 * the sake of readability. In the future the could be used by other AOs based
 * on CoreAudio but not the AUHAL (such as using AudioQueue services).
 */

#include "audio/out/ao_coreaudio_utils.h"
#include "osdep/timer.h"
#include "osdep/endian.h"
#include "audio/format.h"
#include "osdep/mac/compat.h"

#if HAVE_COREAUDIO || HAVE_AVFOUNDATION
#include "audio/out/ao_coreaudio_properties.h"
#include <CoreAudio/HostTime.h>
#else
#include <mach/mach_time.h>
#endif

#if HAVE_COREAUDIO || HAVE_AVFOUNDATION

static bool ca_is_output_device(struct ao *ao, AudioDeviceID dev)
{
    size_t n_buffers;
    AudioBufferList *buffers;
    const ca_scope scope = kAudioDevicePropertyStreamConfiguration;
    OSStatus err = CA_GET_ARY_O(dev, scope, &buffers, &n_buffers);
    if (err != noErr)
        return false;
    talloc_free(buffers);
    return n_buffers > 0;
}

void ca_get_device_list(struct ao *ao, struct ao_device_list *list)
{
    AudioDeviceID *devs;
    size_t n_devs;
    OSStatus err =
        CA_GET_ARY(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                   &devs, &n_devs);
    CHECK_CA_ERROR("Failed to get list of output devices");
    for (int i = 0; i < n_devs; i++) {
        if (!ca_is_output_device(ao, devs[i]))
            continue;
        void *ta_ctx = talloc_new(NULL);
        char *name;
        char *desc;
        err = CA_GET_STR(devs[i], kAudioDevicePropertyDeviceUID, &name);
        if (err != noErr) {
            MP_VERBOSE(ao, "Skipping device %d, which has no UID\n", i);
            talloc_free(ta_ctx);
            continue;
        }
        talloc_steal(ta_ctx, name);
        err = CA_GET_STR(devs[i], kAudioObjectPropertyName, &desc);
        if (err != noErr)
            desc = talloc_strdup(NULL, "Unknown");
        talloc_steal(ta_ctx, desc);
        ao_device_list_add(list, ao, &(struct ao_device_desc){name, desc});
        talloc_free(ta_ctx);
    }
    talloc_free(devs);
coreaudio_error:
    return;
}

OSStatus ca_get_frame_buffer_size(struct ao *ao, AudioDeviceID device)
{
    AudioValueRange value_range = {0, 0};
    UInt32 VariableBufferFrameSizes;

    OSStatus err = CA_GET_O(device, kAudioDevicePropertyBufferFrameSizeRange, &value_range);
    OSStatus err1 = CA_GET_O(device, kAudioDevicePropertyUsesVariableBufferFrameSizes, &VariableBufferFrameSizes);

    if (VariableBufferFrameSizes != 0){
        MP_VERBOSE(ao, "Device I/O buffer size range: %g - %g frames (variable size: %u)\n", 
            value_range.mMinimum, value_range.mMaximum, VariableBufferFrameSizes);
    }else{
        MP_VERBOSE(ao, "Device I/O buffer size range: %g - %g frames (variable size: N/A)\n", 
            value_range.mMinimum, value_range.mMaximum);
    }
    return err & err1;

}

OSStatus ca_get_Device_Transport_Type_and_data_source(struct ao *ao, AudioDeviceID device)
{
    UInt32 Transport_Type;
    OSStatus err = CA_GET_O(device, kAudioDevicePropertyTransportType, &Transport_Type);

    UInt32 SourceID;
    OSStatus err1 = CA_GET_O(device, kAudioDevicePropertyDataSource, &SourceID);

    if (err == noErr){
        if (Transport_Type == kAudioDeviceTransportTypeUnknown){
                MP_VERBOSE(ao, "Device transport type: unknown\n");
        }else{
            // "kAudioDevicePropertyDataSource" only works for limited Transport Type,
            // e.g. not work for USB or Bluetooth connection.
            if (err1 == noErr){
                AudioObjectPropertyAddress nameAddr;
                nameAddr.mSelector = kAudioDevicePropertyDataSourceNameForIDCFString;
                nameAddr.mScope = kAudioObjectPropertyScopeOutput;
                nameAddr.mElement = kAudioObjectPropertyElementMain;

                CFStringRef value = NULL;

                AudioValueTranslation audioValueTranslation;
                audioValueTranslation.mInputDataSize = sizeof(UInt32);
                audioValueTranslation.mOutputData = (void *) &value;
                audioValueTranslation.mOutputDataSize = sizeof(char *);
                audioValueTranslation.mInputData = (void *) &SourceID;

                UInt32 propsize = sizeof(AudioValueTranslation);

                AudioObjectGetPropertyData(device, &nameAddr, 0, NULL, &propsize, &audioValueTranslation);

                char *myCString = cfstr_get_cstr(value);

                MP_VERBOSE(ao, "Device transport type: %s (source: %s)\n",
                    mp_tag_str(CFSwapInt32HostToBig(Transport_Type)), myCString);
            }else{
                MP_VERBOSE(ao, "Device transport type: %s (source: Default)\n", mp_tag_str(CFSwapInt32HostToBig(Transport_Type)));
            }
        }
    }
    return err;
}

OSStatus ca_set_frame_buffer_size(struct ao *ao, AudioDeviceID device, int *buffersize)
{
    // Reference: https://github.com/cmus/cmus/blob/master/op/coreaudio.c
    AudioValueRange value_range = {0, 0};
    UInt32 VariableBufferFrameSizes;
    int buffersize_original;
    OSStatus err1 = CA_GET_O(device, kAudioDevicePropertyBufferFrameSizeRange, &value_range);
    CA_GET_O(device, kAudioDevicePropertyUsesVariableBufferFrameSizes, &VariableBufferFrameSizes);

    if (VariableBufferFrameSizes != 0){
        MP_VERBOSE(ao, "Device I/O buffer size range: %g - %g frames (variable size: %u)\n", 
            value_range.mMinimum, value_range.mMaximum, VariableBufferFrameSizes);
    }else{
        MP_VERBOSE(ao, "Device I/O buffer size range: %g - %g frames (variable size: N/A)\n", 
            value_range.mMinimum, value_range.mMaximum);
    }

    OSStatus err = CA_SET(device, kAudioDevicePropertyBufferFrameSize, buffersize);
    // e.g. 16 bit SPDIF AC-3 has a static buffer size of 1536 frames.
    if (value_range.mMinimum == value_range.mMaximum){
        *buffersize = value_range.mMinimum;
        MP_VERBOSE(ao, "I/O buffer size is %d frames\n", *buffersize);
    }else{
        if ((*buffersize >= value_range.mMinimum) && (*buffersize <= value_range.mMaximum)){
            MP_VERBOSE(ao, "Set I/O buffer size to %d frames\n", *buffersize);
        }else if (*buffersize < value_range.mMinimum){
            buffersize_original = *buffersize;
            *buffersize = value_range.mMinimum;
            MP_VERBOSE(ao, "Target I/O buffer size (%d frames) is invalid, increase to %d frames\n", buffersize_original, *buffersize);
        }else{
            buffersize_original = *buffersize;
            *buffersize = value_range.mMaximum;
            MP_VERBOSE(ao, "Target I/O buffer size (%d frames) is invalid, reduce to %d frames\n", buffersize_original, *buffersize);
        }
    }
    return err & err1;
}

OSStatus ca_get_Terminal_Type(struct ao *ao, AudioDeviceID device)
{
    if (!device)
    return 0;

    UInt32 val = 0;
    UInt32 size = sizeof(UInt32);

    AudioObjectPropertyAddress propertyAddress;
    propertyAddress.mScope    = kAudioObjectPropertyScopeGlobal;
    propertyAddress.mElement  = kAudioObjectPropertyElementMain;
    propertyAddress.mSelector = kAudioStreamPropertyTerminalType;

    OSStatus ret = AudioObjectGetPropertyData(device, &propertyAddress, 0, NULL, &size, &val);

    if (ret == noErr) {
        if (val == kAudioStreamTerminalTypeUnknown){
            MP_VERBOSE(ao, "Stream terminal type: unknown\n");
        }else{
            MP_VERBOSE(ao, "Stream terminal type: %s\n", mp_tag_str_hex(CFSwapInt32HostToBig(val)));
        }

    }
    return ret;
}

// Another way to set largest CoreAudio Frame Buffer Size or 4096, whichever is smaller.
OSStatus SetAudioPowerHintToFavorSavingPower(void)
{
    AudioObjectPropertyAddress theAddress = { kAudioHardwarePropertyPowerHint,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain};
    UInt32 thePowerHint = kAudioHardwarePowerHintFavorSavingPower;
    return AudioObjectSetPropertyData(kAudioObjectSystemObject, &theAddress, 0, NULL, sizeof(UInt32), &thePowerHint);
}

OSStatus ca_IO_Cycle_Usage(struct ao *ao, AudioDeviceID device, Float32 *IOCycleUsage)
{
   OSStatus err = CA_SET(device, kAudioDevicePropertyIOCycleUsage, IOCycleUsage);
   MP_VERBOSE(ao, "Set device I/O Cycle Usage to %g\n", *IOCycleUsage);
   return err;
}

// In exclusive Mode, once audio is played, there is no way to check the current volume.
// Use this to display volume, mute and balance.
// For audiophiles, it is important to keep at 0dB in the digial region.
OSStatus ca_get_ao_volume(struct ao *ao, AudioDeviceID device, UInt32 channel)
{
    if (!device)
    return 0;

    Float32 volume;
    Float32 volumedb;

    int channelmute;
    int submute;

    UInt32 dataSize = sizeof(volume);
    Float32 VirtualMasterBalance;
    Float32 VirtualMasterVolume;
    Float32 SubVolumeScalar;
    Float32 SubVolumeDecibels;
    int mute;

    OSStatus VirtualMasterVolumeresult = CA_GET_O(device, kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
                                                    &VirtualMasterVolume);

    OSStatus VirtualMasterBalanceresult = CA_GET_O(device, kAudioHardwareServiceDeviceProperty_VirtualMainBalance,
                                                    &VirtualMasterBalance);

    OSStatus subvloume = CA_GET_O(device, kAudioDevicePropertySubVolumeScalar, &SubVolumeScalar);

    OSStatus subvloumedb = CA_GET_O(device, kAudioDevicePropertySubVolumeDecibels, &SubVolumeDecibels);

    OSStatus muteresult = CA_GET_O(device, kAudioDevicePropertyMute, &mute);

    OSStatus submuteresult = CA_GET_O(device, kAudioDevicePropertySubMute, &submute);

    if ((muteresult == noErr) && (mute == 1)){
        MP_VERBOSE(ao, "Device is in mute\n");
    }else{
        if (VirtualMasterVolumeresult == noErr){
            MP_VERBOSE(ao, "Virtual main volume: %.2f\n", VirtualMasterVolume);
        }

        if (VirtualMasterBalanceresult == noErr){
            MP_VERBOSE(ao, "Virtual main balance: %g\n", VirtualMasterBalance);
        }

        if ((subvloume == noErr) && (subvloumedb == noErr) && (submuteresult == noErr)){
            if (submute == 1){
                MP_VERBOSE(ao, "LFE channel is in mute\n");
            }else{
                MP_VERBOSE(ao, "LFE volume: %.2f (%.1f dB)\n", SubVolumeScalar, SubVolumeDecibels);
            }
        }

        for (UInt32 j = 0; j <= channel;j++){
            AudioObjectPropertyAddress prop = {kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, j};
            AudioObjectPropertyAddress prop_db = {kAudioDevicePropertyVolumeDecibels, kAudioDevicePropertyScopeOutput, j};
            AudioObjectPropertyAddress prop_mute = {kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput,j};

            if (AudioObjectHasProperty(device, &prop)){
                OSStatus VolumeScalar = AudioObjectGetPropertyData(device, &prop, 0, NULL,
                    &dataSize, &volume);

                OSStatus VolumeDecibels = AudioObjectGetPropertyData(device, &prop_db, 0, NULL,
                    &dataSize, &volumedb);

                OSStatus channel_mute = AudioObjectGetPropertyData(device, &prop_mute, 0, NULL, &dataSize,
                    &channelmute);

                if (j == 0){ // Channel 0  is master, if available
                    if ((VolumeScalar == noErr) && (VolumeDecibels == noErr)){
                        if (channel_mute == noErr){  
                            if (channelmute == 1){
                                MP_VERBOSE(ao, "Main channel is in mute\n");
                            }
                        }
                        MP_VERBOSE(ao, "Main volume: %.2f (%.1f dB)\n", volume, volumedb);
                    }
                }else{
                    if ((VolumeScalar == noErr) && (VolumeDecibels == noErr)){
                        if (channelmute == 1){
                            MP_VERBOSE(ao, "Channel %u is in mute\n", j);
                        }else{
                        MP_VERBOSE(ao, "Channel %u volume: %.2f (%.1f dB)\n", j, volume, volumedb);
                        }
                    }else{
                        MP_VERBOSE(ao, "Channel %u volume is not available\n", j);   
                    }
                }
            }
        }
    }
    return noErr;
}

OSStatus ca_select_device(struct ao *ao, char* name, AudioDeviceID *device)
{
    OSStatus err = noErr;
    *device = kAudioObjectUnknown;

    if (name && name[0]) {
        CFStringRef uid = cfstr_from_cstr(name);
        AudioValueTranslation v = (AudioValueTranslation) {
            .mInputData = &uid,
            .mInputDataSize = sizeof(CFStringRef),
            .mOutputData = device,
            .mOutputDataSize = sizeof(*device),
        };
        uint32_t size = sizeof(AudioValueTranslation);
        AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
            .mSelector = kAudioHardwarePropertyDeviceForUID,
            .mScope    = kAudioObjectPropertyScopeGlobal,
            .mElement  = kAudioObjectPropertyElementMain,
        };
        err = AudioObjectGetPropertyData(
            kAudioObjectSystemObject, &p_addr, 0, 0, &size, &v);
        CFRelease(uid);
        CHECK_CA_ERROR("Unable to query for device UID");

        uint32_t is_alive = 1;
        err = CA_GET(*device, kAudioDevicePropertyDeviceIsAlive, &is_alive);
        CHECK_CA_ERROR("Could not check whether device is alive (invalid device?)");

        if (!is_alive)
            MP_WARN(ao, "Device is not alive!\n");
    } else {
        // device not set by user, get the default one
        err = CA_GET(kAudioObjectSystemObject,
                     kAudioHardwarePropertyDefaultOutputDevice,
                     device);
        CHECK_CA_ERROR("Could not get default audio device");
    }

    if (mp_msg_test(ao->log, MSGL_V)) {
        char *desc;
        char *manufacturer;
        char *UID;
        OSStatus err2 = CA_GET_STR(*device, kAudioObjectPropertyName, &desc);
        OSStatus err3 = CA_GET_STR(*device, kAudioObjectPropertyManufacturer, &manufacturer);
        OSStatus err4 = CA_GET_STR(*device, kAudioDevicePropertyDeviceUID, &UID);
        if (err2 == noErr && err3 == noErr){
            MP_VERBOSE(ao, "Selected audio device: %s (%s)\n", desc, manufacturer);
        }else{
            MP_VERBOSE(ao, "Selected audio device: %s\n", desc);
        }

        if (err2 == noErr && err4 == noErr){
            MP_VERBOSE(ao, "Device ID: 0x%0X (UID: %s)\n", *device, UID);
            talloc_free(desc);
            talloc_free(manufacturer);
            talloc_free(UID);
        }
    }

coreaudio_error:
    return err;
}
#endif

bool check_ca_st(struct ao *ao, int level, OSStatus code, const char *message)
{
    if (code == noErr) return true;

    if (ao)
        mp_msg(ao->log, level, "%s (%s/%d)\n", message, mp_tag_str(code), (int)code);

    return false;
}

static void ca_fill_asbd_raw(AudioStreamBasicDescription *asbd, int mp_format,
                             int samplerate, int num_channels)
{
    asbd->mSampleRate       = samplerate;
    // Set "AC3" for other spdif formats too - unknown if that works.
    asbd->mFormatID         = af_fmt_is_spdif(mp_format) ?
                              kAudioFormat60958AC3 :
                              kAudioFormatLinearPCM;
    asbd->mChannelsPerFrame = num_channels;
    asbd->mBitsPerChannel   = af_fmt_to_bytes(mp_format) * 8;
    asbd->mFormatFlags      = kAudioFormatFlagIsPacked;

    int channels_per_buffer = num_channels;
    if (af_fmt_is_planar(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
        channels_per_buffer = 1;
    }

    if (af_fmt_is_float(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsFloat;
    } else if (!af_fmt_is_unsigned(mp_format)) {
        asbd->mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    }

    if (BYTE_ORDER == BIG_ENDIAN)
        asbd->mFormatFlags |= kAudioFormatFlagIsBigEndian;

    asbd->mFramesPerPacket = 1;
    asbd->mBytesPerPacket = asbd->mBytesPerFrame =
        asbd->mFramesPerPacket * channels_per_buffer *
        (asbd->mBitsPerChannel / 8);
}

void ca_fill_asbd(struct ao *ao, AudioStreamBasicDescription *asbd)
{
    ca_fill_asbd_raw(asbd, ao->format, ao->samplerate, ao->channels.num);
}

bool ca_formatid_is_compressed(uint32_t formatid)
{
    switch (formatid)
    case 'IAC3':
    case 'iac3':
    case  kAudioFormat60958AC3:
    case  kAudioFormatAC3:
        return true;
    return false;
}

// This might be wrong, but for now it's sufficient for us.
static uint32_t ca_normalize_formatid(uint32_t formatID)
{
    return ca_formatid_is_compressed(formatID) ? kAudioFormat60958AC3 : formatID;
}

bool ca_asbd_equals(const AudioStreamBasicDescription *a,
                    const AudioStreamBasicDescription *b,
                    int integer_mode_hack)
{
    bool spdif = ca_formatid_is_compressed(a->mFormatID) &&
                 ca_formatid_is_compressed(b->mFormatID);

    if (integer_mode_hack == 1){
        int flags = kAudioFormatFlagIsFloat | // unpacked 24 bit device is NOT packed.
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

        return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
              a->mBitsPerChannel >= b->mBitsPerChannel && // mpv doesn't have s24, only s32.
              ca_normalize_formatid(a->mFormatID) ==
              ca_normalize_formatid(b->mFormatID) &&
              (spdif || a->mBytesPerPacket == b->mBytesPerPacket) &&
              (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
              a->mSampleRate == b->mSampleRate;

    }else if(integer_mode_hack == 2){
        int flags = kAudioFormatFlagIsNonMixable | kAudioFormatFlagIsFloat |
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

        return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
              a->mBitsPerChannel == b->mBitsPerChannel &&
              ca_normalize_formatid(a->mFormatID) ==
              ca_normalize_formatid(b->mFormatID) &&
              (spdif || a->mBytesPerPacket == b->mBytesPerPacket) &&
              (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
              a->mSampleRate == b->mSampleRate;

    }else if(integer_mode_hack == 3){ // for packed 24 bit device
        int flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat |
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

        return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
              a->mBitsPerChannel >= b->mBitsPerChannel &&
              ca_normalize_formatid(a->mFormatID) ==
              ca_normalize_formatid(b->mFormatID) &&
              (spdif || a->mBytesPerPacket >= b->mBytesPerPacket) && // mpv's is 4, device's is 3.
              (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
              a->mSampleRate == b->mSampleRate;

    }else{
        int flags = kAudioFormatFlagIsPacked | kAudioFormatFlagIsFloat |
        kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian;

        return (a->mFormatFlags & flags) == (b->mFormatFlags & flags) &&
              a->mBitsPerChannel == b->mBitsPerChannel &&
              ca_normalize_formatid(a->mFormatID) ==
              ca_normalize_formatid(b->mFormatID) &&
              (spdif || a->mBytesPerPacket == b->mBytesPerPacket) &&
              (spdif || a->mChannelsPerFrame == b->mChannelsPerFrame) &&
              a->mSampleRate == b->mSampleRate;
    }
}

// Return the AF_FORMAT_* (AF_FORMAT_S16 etc.) corresponding to the asbd.
int ca_asbd_to_mp_format(const AudioStreamBasicDescription *asbd,
                        int integer_mode_hack,
                        int packed_24_hack)
{
    for (int fmt = 1; fmt < AF_FORMAT_COUNT; fmt++) {
        AudioStreamBasicDescription mp_asbd = {0};
        ca_fill_asbd_raw(&mp_asbd, fmt, asbd->mSampleRate, asbd->mChannelsPerFrame);

        if (integer_mode_hack == 1){
            if (packed_24_hack == 1){
                if (ca_asbd_equals(&mp_asbd, asbd, 3))
                return af_fmt_is_spdif(fmt) ? AF_FORMAT_S_AC3 : fmt;
            }else{
                if (ca_asbd_equals(&mp_asbd, asbd, 1))
                return af_fmt_is_spdif(fmt) ? AF_FORMAT_S_AC3 : fmt;
            }
        }else{
            if (ca_asbd_equals(&mp_asbd, asbd, 0))
            return af_fmt_is_spdif(fmt) ? AF_FORMAT_S_AC3 : fmt;
        }
    }
    return 0;
}

void ca_print_asbd(struct ao *ao, const char *description,
                   const AudioStreamBasicDescription *asbd)
{
    uint32_t flags  = asbd->mFormatFlags;
    char *format    = mp_tag_str(CFSwapInt32HostToBig(asbd->mFormatID));
    int mpfmt       = ca_asbd_to_mp_format(asbd, 1, 0);

    MP_VERBOSE(ao,
       "%s%s %" PRIu32 "Bit/%gkHz "
       "[%" PRIu32 "][%" PRIu32 "bpp][%" PRIu32 "fbp]"
       "[%" PRIu32 "bpf][%" PRIu32 "ch] "
       "%s%s%s%s%s(%s)\n",
       description, format, asbd->mBitsPerChannel, asbd->mSampleRate / 1000,
       asbd->mFormatFlags, asbd->mBytesPerPacket, asbd->mFramesPerPacket,
       asbd->mBytesPerFrame, asbd->mChannelsPerFrame,
       (flags & kAudioFormatFlagIsFloat) ? "F " :
       ((flags & kAudioFormatFlagIsSignedInteger) && (flags | kAudioFormatFlagIsFloat)) ? "Int " : "Uint ",
       (flags & kAudioFormatFlagIsPacked) ? "P ":
       ((flags & kAudioFormatFlagIsAlignedHigh) && (flags | kAudioFormatFlagIsPacked)) ? "High ":
       ((flags | kAudioFormatFlagIsAlignedHigh) && (flags | kAudioFormatFlagIsPacked)) ? "Low ":"",
       (flags & kAudioFormatFlagIsBigEndian) ? "BE " : "LE ",
       (flags & kAudioFormatFlagIsNonInterleaved) ? "NonIntl" : "Intl",
       (flags & kAudioFormatFlagIsNonMixable) ? " Nonmix " : " Mix ", // "Unmixable" indicates integer mode.
       mpfmt ? af_fmt_to_str(mpfmt) : "-");
}

// Return whether new is an improvement over old. Assume a higher value means
// better quality, and we always prefer the value closest to the requested one,
// which is still larger than the requested one.
// Equal values prefer the new one (so ca_asbd_is_better() checks other params).
static bool value_is_better(double req, double old, double new)
{
    if (new >= req) {
        return old < req || new <= old;
    } else {
        return old < req && new >= old;
    }
}

// Return whether new is an improvement over old (req is the requested format).
bool ca_asbd_is_better(AudioStreamBasicDescription *req,
                       AudioStreamBasicDescription *old,
                       AudioStreamBasicDescription *new,
                       int mixableflag,
                       int bytesflag)
{
    if (new->mChannelsPerFrame > MP_NUM_CHANNELS)
        return false;
    if (old->mChannelsPerFrame > MP_NUM_CHANNELS)
        return true;
    if (req->mFormatID != new->mFormatID)
        return false;
    if (req->mFormatID != old->mFormatID)
        return true;

    // Force physical format to be 24/32 bit.
    if (bytesflag == 1){
        if (!value_is_better(6, old->mBytesPerFrame,
                         new->mBytesPerFrame))
            return false;
    }else{
        if (!value_is_better(req->mBitsPerChannel, old->mBitsPerChannel,
                             new->mBitsPerChannel))
            return false;
    }

    // Force virtual format to be 32 bit float.
    if (mixableflag == 1){
        if ((req->mFormatFlags & kAudioFormatFlagIsNonMixable) != (new->mFormatFlags & kAudioFormatFlagIsNonMixable))
            return false;
        if ((req->mFormatFlags & kAudioFormatFlagIsNonMixable) != (old->mFormatFlags & kAudioFormatFlagIsNonMixable))
            return true;
    }

    if (!value_is_better(req->mSampleRate, old->mSampleRate, new->mSampleRate))
        return false;

    if (!value_is_better(req->mChannelsPerFrame, old->mChannelsPerFrame,
                         new->mChannelsPerFrame))
        return false;

    return true;
}

int64_t ca_frames_to_ns(struct ao *ao, uint32_t frames)
{
    return MP_TIME_S_TO_NS(frames / (double)ao->samplerate);
}

int64_t ca_get_latency(const AudioTimeStamp *ts)
{
#if HAVE_COREAUDIO || HAVE_AVFOUNDATION
    uint64_t out = AudioConvertHostTimeToNanos(ts->mHostTime);
    uint64_t now = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());

    if (now > out)
        return 0;

    return out - now;
#else
    static mach_timebase_info_data_t timebase;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);

    uint64_t out = ts->mHostTime;
    uint64_t now = mach_absolute_time();

    if (now > out)
        return 0;

    return (out - now) * timebase.numer / timebase.denom;
#endif
}

#if HAVE_COREAUDIO || HAVE_AVFOUNDATION
bool ca_stream_supports_compressed(struct ao *ao, AudioStreamID stream)
{
    AudioStreamRangedDescription *formats = NULL;
    size_t n_formats;

    OSStatus err =
        CA_GET_ARY(stream, kAudioStreamPropertyAvailablePhysicalFormats,
                   &formats, &n_formats);

    CHECK_CA_ERROR("Could not get number of stream formats");

    for (int i = 0; i < n_formats; i++) {
        AudioStreamBasicDescription asbd = formats[i].mFormat;

        ca_print_asbd(ao, "- ", &asbd);

        if (ca_formatid_is_compressed(asbd.mFormatID)) {
            talloc_free(formats);
            return true;
        }
    }

    talloc_free(formats);
coreaudio_error:
    return false;
}

OSStatus ca_lock_device(AudioDeviceID device, pid_t *pid)
{
    *pid = getpid();
    OSStatus err = CA_SET(device, kAudioDevicePropertyHogMode, pid);
    if (err != noErr)
        *pid = -1;

    return err;
}

OSStatus ca_unlock_device(AudioDeviceID device, pid_t *pid)
{
    if (*pid == getpid()) {
        *pid = -1;
        return CA_SET(device, kAudioDevicePropertyHogMode, &pid);
    }
    return noErr;
}

static OSStatus ca_change_mixing(struct ao *ao, AudioDeviceID device,
                                 uint32_t val, bool *changed)
{
    *changed = false;

    AudioObjectPropertyAddress p_addr = (AudioObjectPropertyAddress) {
        .mSelector = kAudioDevicePropertySupportsMixing,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };

    if (AudioObjectHasProperty(device, &p_addr)) {
        OSStatus err;
        Boolean writeable = 0;
        err = CA_SETTABLE(device, kAudioDevicePropertySupportsMixing,
                          &writeable);

        if (!CHECK_CA_WARN("Can't tell if mixing property is settable")) {
            return err;
        }

        if (!writeable)
            return noErr;

        err = CA_SET(device, kAudioDevicePropertySupportsMixing, &val);
        if (err != noErr)
            return err;

        if (!CHECK_CA_WARN("Can't set mix mode")) {
            return err;
        }

        *changed = true;
    }

    return noErr;
}

OSStatus ca_disable_mixing(struct ao *ao, AudioDeviceID device, bool *changed)
{
    return ca_change_mixing(ao, device, 0, changed);
}

OSStatus ca_enable_mixing(struct ao *ao, AudioDeviceID device, bool changed)
{
    if (changed) {
        bool dont_care = false;
        return ca_change_mixing(ao, device, 1, &dont_care);
    }

    return noErr;
}

int64_t ca_get_device_latency_ns(struct ao *ao, AudioDeviceID device)
{
    uint32_t latency_frames = 0;
    uint32_t latency_properties[] = {
        kAudioDevicePropertyLatency,
        kAudioDevicePropertyBufferFrameSize,
        kAudioDevicePropertySafetyOffset,
    };
    for (int n = 0; n < MP_ARRAY_SIZE(latency_properties); n++) {
        uint32_t temp;
        OSStatus err = CA_GET_O(device, latency_properties[n], &temp);
        CHECK_CA_WARN("Cannot get device latency");
        if (err == noErr) {
            latency_frames += temp;
            MP_VERBOSE(ao, "Latency property %s: %d frames\n",
                       mp_tag_str(CFSwapInt32HostToBig(latency_properties[n])), (int)temp);
        }
    }

    double sample_rate = ao->samplerate;
    OSStatus err = CA_GET_O(device, kAudioDevicePropertyNominalSampleRate,
                            &sample_rate);
    CHECK_CA_WARN("Cannot get device sample rate, falling back to AO sample rate!");
    if (err == noErr) {
        MP_VERBOSE(ao, "Device sample rate: %.0f Hz\n", sample_rate);
    }

    return MP_TIME_S_TO_NS(latency_frames / sample_rate);
}

static OSStatus ca_change_format_listener(
    AudioObjectID object, uint32_t n_addresses,
    const AudioObjectPropertyAddress addresses[],
    void *data)
{
    struct coreaudio_cb_sem *sem = data;
    mp_mutex_lock(&sem->mutex);
    mp_cond_broadcast(&sem->cond);
    mp_mutex_unlock(&sem->mutex);
    return noErr;
}

bool ca_change_physical_format_sync(struct ao *ao, AudioStreamID stream,
                                    AudioStreamBasicDescription change_format)
{
    struct coreaudio_cb_sem *sem = ao->priv;

    OSStatus err = noErr;
    bool format_set = false;

    ca_print_asbd(ao, "Setting stream physical format: ", &change_format);

    AudioStreamBasicDescription prev_format;
    err = CA_GET(stream, kAudioStreamPropertyPhysicalFormat, &prev_format);
    CHECK_CA_ERROR("Can't get current physical format");

    ca_print_asbd(ao, "Format in use before switching: ", &prev_format);

    /* Install the callback. */
    AudioObjectPropertyAddress p_addr = {
        .mSelector = kAudioStreamPropertyPhysicalFormat,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };

    err = AudioObjectAddPropertyListener(stream, &p_addr,
                                         ca_change_format_listener,
                                         sem);
    CHECK_CA_ERROR("Can't add property listener during format change");

    /* Change the format. */
    err = CA_SET(stream, kAudioStreamPropertyPhysicalFormat, &change_format);
    CHECK_CA_WARN("Error changing physical format");

    /* The AudioStreamSetProperty is not only asynchronous,
     * it is also not Atomic, in its behaviour. */
    int64_t wait_until = mp_time_ns() + MP_TIME_S_TO_NS(2);
    AudioStreamBasicDescription actual_format = {0};
    while (1) {
        err = CA_GET(stream, kAudioStreamPropertyPhysicalFormat, &actual_format);
        if (!CHECK_CA_WARN("Could not retrieve physical format"))
            break;

        format_set = ca_asbd_equals(&change_format, &actual_format, 2);
        if (format_set)
            break;

        if (mp_cond_timedwait_until(&sem->cond, &sem->mutex,
            wait_until)) {
            MP_VERBOSE(ao, "Reached timeout\n");
            break;
        }
    }

    mp_mutex_unlock(&sem->mutex);

    ca_print_asbd(ao, "Actual format in use: ", &actual_format);

    if (!format_set) {
        MP_WARN(ao, "Changing physical format failed\n");
        // Some drivers just fuck up and get into a broken state. Restore the
        // old format in this case.
        err = CA_SET(stream, kAudioStreamPropertyPhysicalFormat, &prev_format);
        CHECK_CA_WARN("Error restoring physical format");
    }

    err = AudioObjectRemovePropertyListener(stream, &p_addr,
                                            ca_change_format_listener,
                                            sem);
    CHECK_CA_ERROR("Can't remove property listener");

coreaudio_error:
    return format_set;
}
#endif
