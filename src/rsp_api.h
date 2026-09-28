// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The parts of SDRplay's API 3.14 and 3.15 that the module uses, declared
// from the API Specification (v3.15, sections 2 and 3) rather than taken
// from SDRplay's headers, which are not the module's to copy. The layout
// must match the library byte for byte: tests/abi_check.cpp compares every
// size and offset with the official headers where they are installed.
//
// The C enums of the specification are plain 32-bit integers here: a C
// enum is an int in the x86_64, aarch64 and armhf ABIs, both as a struct
// member and as an argument, and ReasonForUpdateT's 0x80000000 only fits an
// unsigned one.
#pragma once

#include <cstdint>

namespace fern::rsp {

using Handle = void*;
using ErrT = int32_t;

namespace err {
constexpr ErrT success = 0;
constexpr ErrT fail = 1;
constexpr ErrT invalid_param = 2;
constexpr ErrT out_of_range = 3;
constexpr ErrT gain_update_error = 4;
constexpr ErrT rf_update_error = 5;
constexpr ErrT fs_update_error = 6;
constexpr ErrT hw_error = 7;
constexpr ErrT aliasing_error = 8;
constexpr ErrT already_initialised = 9;
constexpr ErrT not_initialised = 10;
constexpr ErrT not_enabled = 11;
constexpr ErrT hw_ver_error = 12;
constexpr ErrT out_of_mem_error = 13;
constexpr ErrT service_not_responding = 14;
constexpr ErrT start_pending = 15;
constexpr ErrT stop_pending = 16;
constexpr ErrT invalid_mode = 17;
constexpr ErrT invalid_service_version = 24;
}  // namespace err

constexpr unsigned max_devices = 16;
constexpr unsigned max_serial_length = 64;

// DeviceT.hwVer
namespace hw {
constexpr unsigned char rsp1 = 1;
constexpr unsigned char rsp1a = 255;
constexpr unsigned char rsp2 = 2;  // the RSP2pro too
constexpr unsigned char rspduo = 3;
constexpr unsigned char rspdx = 4;
constexpr unsigned char rsp1b = 6;
constexpr unsigned char rspdx_r2 = 7;
}  // namespace hw

using TunerSelect = int32_t;
namespace tuner {
constexpr TunerSelect neither = 0;
constexpr TunerSelect a = 1;
constexpr TunerSelect b = 2;
constexpr TunerSelect both = 3;
}  // namespace tuner

using RspDuoMode = int32_t;  // a bit set in DeviceT on return from GetDevices
namespace duo_mode {
constexpr RspDuoMode unknown = 0;
constexpr RspDuoMode single_tuner = 1;
constexpr RspDuoMode dual_tuner = 2;
constexpr RspDuoMode master = 4;
constexpr RspDuoMode slave = 8;
}  // namespace duo_mode

// Bandwidths in kHz, whatever the specification's type name says.
using Bw = int32_t;
using IfKhz = int32_t;
namespace if_khz {
constexpr IfKhz zero = 0;
constexpr IfKhz if_0_450 = 450;
constexpr IfKhz if_1_620 = 1620;
constexpr IfKhz if_2_048 = 2048;
}  // namespace if_khz

using LoMode = int32_t;
using MinGainReduction = int32_t;
namespace min_gr {
constexpr MinGainReduction extended = 0;
constexpr MinGainReduction normal = 20;
}  // namespace min_gr
constexpr int max_bb_gr = 59;  // MAX_BB_GR, the most IF gain reduction

using AgcControl = int32_t;
namespace agc {
constexpr AgcControl disable = 0;
constexpr AgcControl hz100 = 1;
constexpr AgcControl hz50 = 2;
constexpr AgcControl hz5 = 3;
constexpr AgcControl ctrl_en = 4;
}  // namespace agc

using AdsbMode = int32_t;
using TransferMode = int32_t;

using Rsp2AntennaSelect = int32_t;
namespace rsp2_antenna {
constexpr Rsp2AntennaSelect a = 5;
constexpr Rsp2AntennaSelect b = 6;
}  // namespace rsp2_antenna

// AMPORT_1 is the Hi-Z input of an RSP2 and of the RSPduo's tuner 1.
using AmPortSelect = int32_t;
namespace am_port {
constexpr AmPortSelect port1 = 1;
constexpr AmPortSelect port2 = 0;
}  // namespace am_port

using RspDxAntennaSelect = int32_t;
namespace rspdx_antenna {
constexpr RspDxAntennaSelect a = 0;
constexpr RspDxAntennaSelect b = 1;
constexpr RspDxAntennaSelect c = 2;
}  // namespace rspdx_antenna

using RspDxHdrModeBw = int32_t;

using ReasonForUpdate = uint32_t;
namespace update {
constexpr ReasonForUpdate none = 0x00000000;
constexpr ReasonForUpdate dev_fs = 0x00000001;
constexpr ReasonForUpdate dev_ppm = 0x00000002;
constexpr ReasonForUpdate rsp1a_bias_t = 0x00000010;
constexpr ReasonForUpdate rsp1a_rf_notch = 0x00000020;
constexpr ReasonForUpdate rsp1a_rf_dab_notch = 0x00000040;
constexpr ReasonForUpdate rsp2_bias_t = 0x00000080;
constexpr ReasonForUpdate rsp2_am_port = 0x00000100;
constexpr ReasonForUpdate rsp2_antenna = 0x00000200;
constexpr ReasonForUpdate rsp2_rf_notch = 0x00000400;
constexpr ReasonForUpdate tuner_gr = 0x00008000;
constexpr ReasonForUpdate tuner_gr_limits = 0x00010000;
constexpr ReasonForUpdate tuner_frf = 0x00020000;
constexpr ReasonForUpdate tuner_bw_type = 0x00040000;
constexpr ReasonForUpdate tuner_if_type = 0x00080000;
constexpr ReasonForUpdate ctrl_dc_offset_iq_imbalance = 0x00400000;
constexpr ReasonForUpdate ctrl_decimation = 0x00800000;
constexpr ReasonForUpdate ctrl_agc = 0x01000000;
constexpr ReasonForUpdate ctrl_overload_msg_ack = 0x04000000;
constexpr ReasonForUpdate rspduo_bias_t = 0x08000000;
constexpr ReasonForUpdate rspduo_am_port = 0x10000000;
constexpr ReasonForUpdate rspduo_tuner1_am_notch = 0x20000000;
constexpr ReasonForUpdate rspduo_rf_notch = 0x40000000;
constexpr ReasonForUpdate rspduo_rf_dab_notch = 0x80000000;
}  // namespace update

using ReasonForUpdateExt1 = uint32_t;
namespace update_ext1 {
constexpr ReasonForUpdateExt1 none = 0x00000000;
constexpr ReasonForUpdateExt1 rspdx_hdr_enable = 0x00000001;
constexpr ReasonForUpdateExt1 rspdx_bias_t = 0x00000002;
constexpr ReasonForUpdateExt1 rspdx_antenna = 0x00000004;
constexpr ReasonForUpdateExt1 rspdx_rf_notch = 0x00000008;
constexpr ReasonForUpdateExt1 rspdx_rf_dab_notch = 0x00000010;
constexpr ReasonForUpdateExt1 rspdx_hdr_bw = 0x00000020;
}  // namespace update_ext1

using EventId = int32_t;
namespace event {
constexpr EventId gain_change = 0;
constexpr EventId power_overload_change = 1;
constexpr EventId device_removed = 2;
constexpr EventId rspduo_mode_change = 3;
constexpr EventId device_failure = 4;
}  // namespace event

using PowerOverloadEvent = int32_t;
namespace overload {
constexpr PowerOverloadEvent detected = 0;
constexpr PowerOverloadEvent corrected = 1;
}  // namespace overload

// sdrplay_api.h
struct DeviceT {
    char SerNo[max_serial_length];
    unsigned char hwVer;
    TunerSelect tuner;
    RspDuoMode rspDuoMode;
    unsigned char valid;
    double rspDuoSampleFreq;
    Handle dev;
};

struct ErrorInfoT {
    char file[256];
    char function[256];
    int line;
    char message[1024];
};

// sdrplay_api_dev.h, with the per-model device parameters of
// sdrplay_api_rsp1a.h, _rsp2.h, _rspDuo.h and _rspDx.h.
struct FsFreqT {
    double fsHz;
    unsigned char syncUpdate;
    unsigned char reCal;
};

struct SyncUpdateT {
    unsigned int sampleNum;
    unsigned int period;
};

struct ResetFlagsT {
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
    unsigned char resetFsUpdate;
};

struct Rsp1aParamsT {
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};

struct Rsp2ParamsT {
    unsigned char extRefOutputEn;
};

struct RspDuoParamsT {
    int extRefOutputEn;
};

struct RspDxParamsT {
    unsigned char hdrEnable;
    unsigned char biasTEnable;
    RspDxAntennaSelect antennaSel;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
};

struct DevParamsT {
    double ppm;
    FsFreqT fsFreq;
    SyncUpdateT syncUpdate;
    ResetFlagsT resetFlags;
    TransferMode mode;
    unsigned int samplesPerPkt;
    Rsp1aParamsT rsp1aParams;
    Rsp2ParamsT rsp2Params;
    RspDuoParamsT rspDuoParams;
    RspDxParamsT rspDxParams;
};

// sdrplay_api_tuner.h
struct GainValuesT {
    float curr;
    float max;
    float min;
};

struct GainT {
    int gRdB;
    unsigned char LNAstate;
    unsigned char syncUpdate;
    MinGainReduction minGr;
    GainValuesT gainVals;
};

struct RfFreqT {
    double rfHz;
    unsigned char syncUpdate;
};

struct DcOffsetTunerT {
    unsigned char dcCal;
    unsigned char speedUp;
    int trackTime;
    int refreshRateTime;
};

struct TunerParamsT {
    Bw bwType;
    IfKhz ifType;
    LoMode loMode;
    GainT gain;
    RfFreqT rfFreq;
    DcOffsetTunerT dcOffsetTuner;
};

// sdrplay_api_control.h
struct DcOffsetT {
    unsigned char DCenable;
    unsigned char IQenable;
};

struct DecimationT {
    unsigned char enable;
    unsigned char decimationFactor;
    unsigned char wideBandSignal;
};

struct AgcT {
    AgcControl enable;
    int setPoint_dBfs;
    unsigned short attack_ms;
    unsigned short decay_ms;
    unsigned short decay_delay_ms;
    unsigned short decay_threshold_dB;
    int syncUpdate;
};

struct ControlParamsT {
    DcOffsetT dcOffset;
    DecimationT decimation;
    AgcT agc;
    AdsbMode adsbMode;
};

// The per-model tuner parameters.
struct Rsp1aTunerParamsT {
    unsigned char biasTEnable;
};

struct Rsp2TunerParamsT {
    unsigned char biasTEnable;
    AmPortSelect amPortSel;
    Rsp2AntennaSelect antennaSel;
    unsigned char rfNotchEnable;
};

struct RspDuoResetSlaveFlagsT {
    unsigned char resetGainUpdate;
    unsigned char resetRfUpdate;
};

struct RspDuoTunerParamsT {
    unsigned char biasTEnable;
    AmPortSelect tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
    RspDuoResetSlaveFlagsT resetSlaveFlags;
};

struct RspDxTunerParamsT {
    RspDxHdrModeBw hdrBw;
};

// sdrplay_api_rx_channel.h
struct RxChannelParamsT {
    TunerParamsT tunerParams;
    ControlParamsT ctrlParams;
    Rsp1aTunerParamsT rsp1aTunerParams;
    Rsp2TunerParamsT rsp2TunerParams;
    RspDuoTunerParamsT rspDuoTunerParams;
    RspDxTunerParamsT rspDxTunerParams;
};

struct DeviceParamsT {
    DevParamsT* devParams;  // NULL for an RSPduo slave
    RxChannelParamsT* rxChannelA;
    RxChannelParamsT* rxChannelB;
};

// sdrplay_api_callback.h
struct GainCbParamT {
    unsigned int gRdB;
    unsigned int lnaGRdB;  // dB, not the LNA state
    double currGain;
};

struct PowerOverloadCbParamT {
    PowerOverloadEvent powerOverloadChangeType;
};

struct RspDuoModeCbParamT {
    int32_t modeChangeType;
};

union EventParamsT {
    GainCbParamT gainParams;
    PowerOverloadCbParamT powerOverloadParams;
    RspDuoModeCbParamT rspDuoModeParams;
};

struct StreamCbParamsT {
    unsigned int firstSampleNum;
    int grChanged;
    int rfChanged;
    int fsChanged;
    unsigned int numSamples;
};

extern "C" {
using StreamCallback = void (*)(short* xi, short* xq, StreamCbParamsT* params, unsigned int numSamples,
                                unsigned int reset, void* cbContext);
using EventCallback = void (*)(EventId eventId, TunerSelect tuner, EventParamsT* params, void* cbContext);
}

struct CallbackFnsT {
    StreamCallback StreamACbFn;
    StreamCallback StreamBCbFn;
    EventCallback EventCbFn;
};

// The functions, as the library exports them.
extern "C" {
using OpenFn = ErrT (*)();
using CloseFn = ErrT (*)();
using ApiVersionFn = ErrT (*)(float* apiVer);
using LockDeviceApiFn = ErrT (*)();
using UnlockDeviceApiFn = ErrT (*)();
using GetDevicesFn = ErrT (*)(DeviceT* devices, unsigned int* numDevs, unsigned int maxDevs);
using SelectDeviceFn = ErrT (*)(DeviceT* device);
using ReleaseDeviceFn = ErrT (*)(DeviceT* device);
using GetErrorStringFn = const char* (*)(ErrT err);
using GetLastErrorFn = ErrorInfoT* (*)(DeviceT* device);
using GetDeviceParamsFn = ErrT (*)(Handle dev, DeviceParamsT** deviceParams);
using InitFn = ErrT (*)(Handle dev, CallbackFnsT* callbackFns, void* cbContext);
using UninitFn = ErrT (*)(Handle dev);
using UpdateFn = ErrT (*)(Handle dev, TunerSelect tuner, ReasonForUpdate reasonForUpdate,
                          ReasonForUpdateExt1 reasonForUpdateExt1);
}

}  // namespace fern::rsp
