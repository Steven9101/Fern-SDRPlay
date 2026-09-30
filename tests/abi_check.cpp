// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// Compiled, never run: it fails to compile when the module's own
// declarations in src/rsp_api.h lay out a struct, a field or a constant
// differently from SDRplay's official headers. The build compiles it
// wherever those headers are installed (make abi-check), so that a new API
// version that moves a field is caught before a module built for the old
// one reads the wrong bytes.
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include <sdrplay_api.h>

#include "rsp_api.h"

namespace r = fern::rsp;

#define SAME_SIZE(ours, theirs) static_assert(sizeof(r::ours) == sizeof(theirs), #ours " differs in size")
#define SAME_FIELD(ours, theirs, field)                                                  \
    static_assert(offsetof(r::ours, field) == offsetof(theirs, field) &&                 \
                      sizeof(r::ours::field) == sizeof(theirs::field),                   \
                  #ours "." #field " differs in offset or size")
#define SAME_VALUE(ours, theirs) static_assert(static_cast<long long>(ours) == static_cast<long long>(theirs), \
                                               #ours " differs from " #theirs)

// The two versions whose layout the module accepts at run time.
static_assert(SDRPLAY_API_VERSION > 3.139f && SDRPLAY_API_VERSION < 3.151f,
              "these headers are neither 3.14 nor 3.15; check every layout below before accepting a new version");

SAME_VALUE(r::max_devices, SDRPLAY_MAX_DEVICES);
SAME_VALUE(r::max_serial_length, SDRPLAY_MAX_SER_NO_LEN);
SAME_VALUE(r::max_bb_gr, MAX_BB_GR);
SAME_VALUE(r::hw::rsp1, SDRPLAY_RSP1_ID);
SAME_VALUE(r::hw::rsp1a, SDRPLAY_RSP1A_ID);
SAME_VALUE(r::hw::rsp2, SDRPLAY_RSP2_ID);
SAME_VALUE(r::hw::rspduo, SDRPLAY_RSPduo_ID);
SAME_VALUE(r::hw::rspdx, SDRPLAY_RSPdx_ID);
SAME_VALUE(r::hw::rsp1b, SDRPLAY_RSP1B_ID);
// 3.15 added the RSPdx-R2's id; 3.14's headers lack it.
#ifdef SDRPLAY_RSPdxR2_ID
SAME_VALUE(r::hw::rspdx_r2, SDRPLAY_RSPdxR2_ID);
#else
static_assert(SDRPLAY_API_VERSION < 3.141f, "headers of 3.15 or later without SDRPLAY_RSPdxR2_ID");
#endif

// The enums, as integers: the module passes them as 32-bit values.
static_assert(sizeof(sdrplay_api_ErrT) == sizeof(r::ErrT), "ErrT");
static_assert(sizeof(sdrplay_api_ReasonForUpdateT) == sizeof(r::ReasonForUpdate), "ReasonForUpdateT");
static_assert(sizeof(sdrplay_api_ReasonForUpdateExtension1T) == sizeof(r::ReasonForUpdateExt1), "Ext1");
static_assert(sizeof(sdrplay_api_TunerSelectT) == sizeof(r::TunerSelect), "TunerSelectT");
static_assert(sizeof(sdrplay_api_EventT) == sizeof(r::EventId), "EventT");
SAME_VALUE(r::err::success, sdrplay_api_Success);
SAME_VALUE(r::err::fail, sdrplay_api_Fail);
SAME_VALUE(r::err::invalid_param, sdrplay_api_InvalidParam);
SAME_VALUE(r::err::out_of_range, sdrplay_api_OutOfRange);
SAME_VALUE(r::err::gain_update_error, sdrplay_api_GainUpdateError);
SAME_VALUE(r::err::rf_update_error, sdrplay_api_RfUpdateError);
SAME_VALUE(r::err::fs_update_error, sdrplay_api_FsUpdateError);
SAME_VALUE(r::err::hw_error, sdrplay_api_HwError);
SAME_VALUE(r::err::aliasing_error, sdrplay_api_AliasingError);
SAME_VALUE(r::err::already_initialised, sdrplay_api_AlreadyInitialised);
SAME_VALUE(r::err::not_initialised, sdrplay_api_NotInitialised);
SAME_VALUE(r::err::not_enabled, sdrplay_api_NotEnabled);
SAME_VALUE(r::err::hw_ver_error, sdrplay_api_HwVerError);
SAME_VALUE(r::err::out_of_mem_error, sdrplay_api_OutOfMemError);
SAME_VALUE(r::err::service_not_responding, sdrplay_api_ServiceNotResponding);
SAME_VALUE(r::err::start_pending, sdrplay_api_StartPending);
SAME_VALUE(r::err::stop_pending, sdrplay_api_StopPending);
SAME_VALUE(r::err::invalid_mode, sdrplay_api_InvalidMode);
SAME_VALUE(r::err::invalid_service_version, sdrplay_api_InvalidServiceVersion);
SAME_VALUE(r::tuner::neither, sdrplay_api_Tuner_Neither);
SAME_VALUE(r::tuner::a, sdrplay_api_Tuner_A);
SAME_VALUE(r::tuner::b, sdrplay_api_Tuner_B);
SAME_VALUE(r::tuner::both, sdrplay_api_Tuner_Both);
SAME_VALUE(r::duo_mode::single_tuner, sdrplay_api_RspDuoMode_Single_Tuner);
SAME_VALUE(r::duo_mode::dual_tuner, sdrplay_api_RspDuoMode_Dual_Tuner);
SAME_VALUE(r::duo_mode::master, sdrplay_api_RspDuoMode_Master);
SAME_VALUE(r::duo_mode::slave, sdrplay_api_RspDuoMode_Slave);
SAME_VALUE(r::if_khz::zero, sdrplay_api_IF_Zero);
SAME_VALUE(r::if_khz::if_0_450, sdrplay_api_IF_0_450);
SAME_VALUE(r::if_khz::if_1_620, sdrplay_api_IF_1_620);
SAME_VALUE(r::if_khz::if_2_048, sdrplay_api_IF_2_048);
SAME_VALUE(200, sdrplay_api_BW_0_200);
SAME_VALUE(300, sdrplay_api_BW_0_300);
SAME_VALUE(600, sdrplay_api_BW_0_600);
SAME_VALUE(1536, sdrplay_api_BW_1_536);
SAME_VALUE(5000, sdrplay_api_BW_5_000);
SAME_VALUE(6000, sdrplay_api_BW_6_000);
SAME_VALUE(7000, sdrplay_api_BW_7_000);
SAME_VALUE(8000, sdrplay_api_BW_8_000);
SAME_VALUE(r::min_gr::extended, sdrplay_api_EXTENDED_MIN_GR);
SAME_VALUE(r::min_gr::normal, sdrplay_api_NORMAL_MIN_GR);
SAME_VALUE(r::agc::disable, sdrplay_api_AGC_DISABLE);
SAME_VALUE(r::agc::hz100, sdrplay_api_AGC_100HZ);
SAME_VALUE(r::agc::hz50, sdrplay_api_AGC_50HZ);
SAME_VALUE(r::agc::hz5, sdrplay_api_AGC_5HZ);
SAME_VALUE(r::agc::ctrl_en, sdrplay_api_AGC_CTRL_EN);
SAME_VALUE(r::rsp2_antenna::a, sdrplay_api_Rsp2_ANTENNA_A);
SAME_VALUE(r::rsp2_antenna::b, sdrplay_api_Rsp2_ANTENNA_B);
SAME_VALUE(r::am_port::port1, sdrplay_api_Rsp2_AMPORT_1);
SAME_VALUE(r::am_port::port2, sdrplay_api_Rsp2_AMPORT_2);
SAME_VALUE(r::am_port::port1, sdrplay_api_RspDuo_AMPORT_1);
SAME_VALUE(r::am_port::port2, sdrplay_api_RspDuo_AMPORT_2);
SAME_VALUE(r::rspdx_antenna::a, sdrplay_api_RspDx_ANTENNA_A);
SAME_VALUE(r::rspdx_antenna::b, sdrplay_api_RspDx_ANTENNA_B);
SAME_VALUE(r::rspdx_antenna::c, sdrplay_api_RspDx_ANTENNA_C);
SAME_VALUE(r::update::none, sdrplay_api_Update_None);
SAME_VALUE(r::update::dev_fs, sdrplay_api_Update_Dev_Fs);
SAME_VALUE(r::update::dev_ppm, sdrplay_api_Update_Dev_Ppm);
SAME_VALUE(r::update::rsp1a_bias_t, sdrplay_api_Update_Rsp1a_BiasTControl);
SAME_VALUE(r::update::rsp1a_rf_notch, sdrplay_api_Update_Rsp1a_RfNotchControl);
SAME_VALUE(r::update::rsp1a_rf_dab_notch, sdrplay_api_Update_Rsp1a_RfDabNotchControl);
SAME_VALUE(r::update::rsp2_bias_t, sdrplay_api_Update_Rsp2_BiasTControl);
SAME_VALUE(r::update::rsp2_am_port, sdrplay_api_Update_Rsp2_AmPortSelect);
SAME_VALUE(r::update::rsp2_antenna, sdrplay_api_Update_Rsp2_AntennaControl);
SAME_VALUE(r::update::rsp2_rf_notch, sdrplay_api_Update_Rsp2_RfNotchControl);
SAME_VALUE(r::update::tuner_gr, sdrplay_api_Update_Tuner_Gr);
SAME_VALUE(r::update::tuner_gr_limits, sdrplay_api_Update_Tuner_GrLimits);
SAME_VALUE(r::update::tuner_frf, sdrplay_api_Update_Tuner_Frf);
SAME_VALUE(r::update::tuner_bw_type, sdrplay_api_Update_Tuner_BwType);
SAME_VALUE(r::update::tuner_if_type, sdrplay_api_Update_Tuner_IfType);
SAME_VALUE(r::update::ctrl_dc_offset_iq_imbalance, sdrplay_api_Update_Ctrl_DCoffsetIQimbalance);
SAME_VALUE(r::update::ctrl_decimation, sdrplay_api_Update_Ctrl_Decimation);
SAME_VALUE(r::update::ctrl_agc, sdrplay_api_Update_Ctrl_Agc);
SAME_VALUE(r::update::ctrl_overload_msg_ack, sdrplay_api_Update_Ctrl_OverloadMsgAck);
SAME_VALUE(r::update::rspduo_bias_t, sdrplay_api_Update_RspDuo_BiasTControl);
SAME_VALUE(r::update::rspduo_am_port, sdrplay_api_Update_RspDuo_AmPortSelect);
SAME_VALUE(r::update::rspduo_tuner1_am_notch, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl);
SAME_VALUE(r::update::rspduo_rf_notch, sdrplay_api_Update_RspDuo_RfNotchControl);
SAME_VALUE(r::update::rspduo_rf_dab_notch, sdrplay_api_Update_RspDuo_RfDabNotchControl);
SAME_VALUE(r::update_ext1::none, sdrplay_api_Update_Ext1_None);
SAME_VALUE(r::update_ext1::rspdx_hdr_enable, sdrplay_api_Update_RspDx_HdrEnable);
SAME_VALUE(r::update_ext1::rspdx_bias_t, sdrplay_api_Update_RspDx_BiasTControl);
SAME_VALUE(r::update_ext1::rspdx_antenna, sdrplay_api_Update_RspDx_AntennaControl);
SAME_VALUE(r::update_ext1::rspdx_rf_notch, sdrplay_api_Update_RspDx_RfNotchControl);
SAME_VALUE(r::update_ext1::rspdx_rf_dab_notch, sdrplay_api_Update_RspDx_RfDabNotchControl);
SAME_VALUE(r::update_ext1::rspdx_hdr_bw, sdrplay_api_Update_RspDx_HdrBw);
SAME_VALUE(r::event::gain_change, sdrplay_api_GainChange);
SAME_VALUE(r::event::power_overload_change, sdrplay_api_PowerOverloadChange);
SAME_VALUE(r::event::device_removed, sdrplay_api_DeviceRemoved);
SAME_VALUE(r::event::rspduo_mode_change, sdrplay_api_RspDuoModeChange);
SAME_VALUE(r::event::device_failure, sdrplay_api_DeviceFailure);
SAME_VALUE(r::overload::detected, sdrplay_api_Overload_Detected);
SAME_VALUE(r::overload::corrected, sdrplay_api_Overload_Corrected);

SAME_SIZE(DeviceT, sdrplay_api_DeviceT);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, SerNo);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, hwVer);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, tuner);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, rspDuoMode);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, valid);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, rspDuoSampleFreq);
SAME_FIELD(DeviceT, sdrplay_api_DeviceT, dev);

SAME_SIZE(ErrorInfoT, sdrplay_api_ErrorInfoT);
SAME_FIELD(ErrorInfoT, sdrplay_api_ErrorInfoT, file);
SAME_FIELD(ErrorInfoT, sdrplay_api_ErrorInfoT, function);
SAME_FIELD(ErrorInfoT, sdrplay_api_ErrorInfoT, line);
SAME_FIELD(ErrorInfoT, sdrplay_api_ErrorInfoT, message);

SAME_SIZE(FsFreqT, sdrplay_api_FsFreqT);
SAME_FIELD(FsFreqT, sdrplay_api_FsFreqT, fsHz);
SAME_SIZE(SyncUpdateT, sdrplay_api_SyncUpdateT);
SAME_SIZE(ResetFlagsT, sdrplay_api_ResetFlagsT);
SAME_SIZE(Rsp1aParamsT, sdrplay_api_Rsp1aParamsT);
SAME_FIELD(Rsp1aParamsT, sdrplay_api_Rsp1aParamsT, rfNotchEnable);
SAME_FIELD(Rsp1aParamsT, sdrplay_api_Rsp1aParamsT, rfDabNotchEnable);
SAME_SIZE(Rsp2ParamsT, sdrplay_api_Rsp2ParamsT);
SAME_SIZE(RspDuoParamsT, sdrplay_api_RspDuoParamsT);
SAME_SIZE(RspDxParamsT, sdrplay_api_RspDxParamsT);
SAME_FIELD(RspDxParamsT, sdrplay_api_RspDxParamsT, hdrEnable);
SAME_FIELD(RspDxParamsT, sdrplay_api_RspDxParamsT, biasTEnable);
SAME_FIELD(RspDxParamsT, sdrplay_api_RspDxParamsT, antennaSel);
SAME_FIELD(RspDxParamsT, sdrplay_api_RspDxParamsT, rfNotchEnable);
SAME_FIELD(RspDxParamsT, sdrplay_api_RspDxParamsT, rfDabNotchEnable);

SAME_SIZE(DevParamsT, sdrplay_api_DevParamsT);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, ppm);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, fsFreq);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, syncUpdate);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, resetFlags);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, mode);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, samplesPerPkt);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, rsp1aParams);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, rsp2Params);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, rspDuoParams);
SAME_FIELD(DevParamsT, sdrplay_api_DevParamsT, rspDxParams);

SAME_SIZE(GainValuesT, sdrplay_api_GainValuesT);
SAME_SIZE(GainT, sdrplay_api_GainT);
SAME_FIELD(GainT, sdrplay_api_GainT, gRdB);
SAME_FIELD(GainT, sdrplay_api_GainT, LNAstate);
SAME_FIELD(GainT, sdrplay_api_GainT, syncUpdate);
SAME_FIELD(GainT, sdrplay_api_GainT, minGr);
SAME_FIELD(GainT, sdrplay_api_GainT, gainVals);
SAME_SIZE(RfFreqT, sdrplay_api_RfFreqT);
SAME_FIELD(RfFreqT, sdrplay_api_RfFreqT, rfHz);
SAME_SIZE(DcOffsetTunerT, sdrplay_api_DcOffsetTunerT);
SAME_SIZE(TunerParamsT, sdrplay_api_TunerParamsT);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, bwType);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, ifType);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, loMode);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, gain);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, rfFreq);
SAME_FIELD(TunerParamsT, sdrplay_api_TunerParamsT, dcOffsetTuner);

SAME_SIZE(DcOffsetT, sdrplay_api_DcOffsetT);
SAME_FIELD(DcOffsetT, sdrplay_api_DcOffsetT, DCenable);
SAME_FIELD(DcOffsetT, sdrplay_api_DcOffsetT, IQenable);
SAME_SIZE(DecimationT, sdrplay_api_DecimationT);
SAME_FIELD(DecimationT, sdrplay_api_DecimationT, enable);
SAME_FIELD(DecimationT, sdrplay_api_DecimationT, decimationFactor);
SAME_FIELD(DecimationT, sdrplay_api_DecimationT, wideBandSignal);
SAME_SIZE(AgcT, sdrplay_api_AgcT);
SAME_FIELD(AgcT, sdrplay_api_AgcT, enable);
SAME_FIELD(AgcT, sdrplay_api_AgcT, setPoint_dBfs);
SAME_FIELD(AgcT, sdrplay_api_AgcT, attack_ms);
SAME_FIELD(AgcT, sdrplay_api_AgcT, decay_ms);
SAME_FIELD(AgcT, sdrplay_api_AgcT, decay_delay_ms);
SAME_FIELD(AgcT, sdrplay_api_AgcT, decay_threshold_dB);
SAME_FIELD(AgcT, sdrplay_api_AgcT, syncUpdate);
SAME_SIZE(ControlParamsT, sdrplay_api_ControlParamsT);
SAME_FIELD(ControlParamsT, sdrplay_api_ControlParamsT, dcOffset);
SAME_FIELD(ControlParamsT, sdrplay_api_ControlParamsT, decimation);
SAME_FIELD(ControlParamsT, sdrplay_api_ControlParamsT, agc);
SAME_FIELD(ControlParamsT, sdrplay_api_ControlParamsT, adsbMode);

SAME_SIZE(Rsp1aTunerParamsT, sdrplay_api_Rsp1aTunerParamsT);
SAME_SIZE(Rsp2TunerParamsT, sdrplay_api_Rsp2TunerParamsT);
SAME_FIELD(Rsp2TunerParamsT, sdrplay_api_Rsp2TunerParamsT, biasTEnable);
SAME_FIELD(Rsp2TunerParamsT, sdrplay_api_Rsp2TunerParamsT, amPortSel);
SAME_FIELD(Rsp2TunerParamsT, sdrplay_api_Rsp2TunerParamsT, antennaSel);
SAME_FIELD(Rsp2TunerParamsT, sdrplay_api_Rsp2TunerParamsT, rfNotchEnable);
SAME_SIZE(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, biasTEnable);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, tuner1AmPortSel);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, tuner1AmNotchEnable);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, rfNotchEnable);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, rfDabNotchEnable);
SAME_FIELD(RspDuoTunerParamsT, sdrplay_api_RspDuoTunerParamsT, resetSlaveFlags);
SAME_SIZE(RspDxTunerParamsT, sdrplay_api_RspDxTunerParamsT);

SAME_SIZE(RxChannelParamsT, sdrplay_api_RxChannelParamsT);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, tunerParams);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, ctrlParams);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, rsp1aTunerParams);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, rsp2TunerParams);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, rspDuoTunerParams);
SAME_FIELD(RxChannelParamsT, sdrplay_api_RxChannelParamsT, rspDxTunerParams);

SAME_SIZE(DeviceParamsT, sdrplay_api_DeviceParamsT);
SAME_FIELD(DeviceParamsT, sdrplay_api_DeviceParamsT, devParams);
SAME_FIELD(DeviceParamsT, sdrplay_api_DeviceParamsT, rxChannelA);
SAME_FIELD(DeviceParamsT, sdrplay_api_DeviceParamsT, rxChannelB);

SAME_SIZE(GainCbParamT, sdrplay_api_GainCbParamT);
SAME_FIELD(GainCbParamT, sdrplay_api_GainCbParamT, gRdB);
SAME_FIELD(GainCbParamT, sdrplay_api_GainCbParamT, lnaGRdB);
SAME_FIELD(GainCbParamT, sdrplay_api_GainCbParamT, currGain);
SAME_SIZE(PowerOverloadCbParamT, sdrplay_api_PowerOverloadCbParamT);
SAME_SIZE(RspDuoModeCbParamT, sdrplay_api_RspDuoModeCbParamT);
SAME_SIZE(EventParamsT, sdrplay_api_EventParamsT);
SAME_SIZE(StreamCbParamsT, sdrplay_api_StreamCbParamsT);
SAME_FIELD(StreamCbParamsT, sdrplay_api_StreamCbParamsT, firstSampleNum);
SAME_FIELD(StreamCbParamsT, sdrplay_api_StreamCbParamsT, grChanged);
SAME_FIELD(StreamCbParamsT, sdrplay_api_StreamCbParamsT, rfChanged);
SAME_FIELD(StreamCbParamsT, sdrplay_api_StreamCbParamsT, fsChanged);
SAME_FIELD(StreamCbParamsT, sdrplay_api_StreamCbParamsT, numSamples);
SAME_SIZE(CallbackFnsT, sdrplay_api_CallbackFnsT);
SAME_FIELD(CallbackFnsT, sdrplay_api_CallbackFnsT, StreamACbFn);
SAME_FIELD(CallbackFnsT, sdrplay_api_CallbackFnsT, StreamBCbFn);
SAME_FIELD(CallbackFnsT, sdrplay_api_CallbackFnsT, EventCbFn);

// The functions. Their types cannot be compared as a whole, since the
// official ones use the enums where src/rsp_api.h uses integers of the same
// size, so they are compared part by part: the number of parameters, and
// for the result and each parameter its size and kind (pointer, floating
// point, integer or enum), through pointers down to the size of what they
// point at.
namespace shape {

template <typename T>
constexpr int kind() {
    return std::is_pointer<T>::value ? 1
           : std::is_floating_point<T>::value ? 2
           : (std::is_integral<T>::value || std::is_enum<T>::value) ? 3
                                                                      : 4;
}

template <typename T, typename U>
constexpr bool same() {
    if constexpr (std::is_void<T>::value || std::is_void<U>::value) {
        return std::is_void<T>::value && std::is_void<U>::value;
    } else if constexpr (std::is_pointer<T>::value && std::is_pointer<U>::value) {
        using PT = typename std::remove_pointer<T>::type;
        using PU = typename std::remove_pointer<U>::type;
        using T0 = typename std::remove_cv<PT>::type;
        using U0 = typename std::remove_cv<PU>::type;
        if (std::is_const<PT>::value != std::is_const<PU>::value)
            return false;
        if constexpr (std::is_void<T0>::value || std::is_void<U0>::value)
            return std::is_void<T0>::value && std::is_void<U0>::value;
        else if constexpr (std::is_pointer<T0>::value || std::is_pointer<U0>::value)
            return same<T0, U0>();
        else
            return sizeof(T0) == sizeof(U0) && kind<T0>() == kind<U0>();
    } else {
        return sizeof(T) == sizeof(U) && kind<T>() == kind<U>();
    }
}

template <typename F>
struct Fn;
template <typename R, typename... A>
struct Fn<R (*)(A...)> {
    using result = R;
    using params = std::tuple<A...>;
    static constexpr std::size_t count = sizeof...(A);
};

template <typename F, typename G, std::size_t... I>
constexpr bool same_params(std::index_sequence<I...>) {
    return (same<typename std::tuple_element<I, typename Fn<F>::params>::type,
                 typename std::tuple_element<I, typename Fn<G>::params>::type>() &&
            ...);
}

template <typename F, typename G>
constexpr bool same_prototype() {
    if constexpr (Fn<F>::count != Fn<G>::count)
        return false;
    else
        return same<typename Fn<F>::result, typename Fn<G>::result>() &&
               same_params<F, G>(std::make_index_sequence<Fn<F>::count>{});
}

}  // namespace shape

// Against both the official function pointer types and the functions the
// official header declares.
#define SAME_FUNCTION(ours, name)                                                                       \
    static_assert(shape::same_prototype<r::ours, name##_t>(), #name " differs from " #name "_t");       \
    static_assert(shape::same_prototype<r::ours, decltype(&name)>(), #name " differs from its declaration")

SAME_FUNCTION(OpenFn, sdrplay_api_Open);
SAME_FUNCTION(CloseFn, sdrplay_api_Close);
SAME_FUNCTION(ApiVersionFn, sdrplay_api_ApiVersion);
SAME_FUNCTION(LockDeviceApiFn, sdrplay_api_LockDeviceApi);
SAME_FUNCTION(UnlockDeviceApiFn, sdrplay_api_UnlockDeviceApi);
SAME_FUNCTION(GetDevicesFn, sdrplay_api_GetDevices);
SAME_FUNCTION(SelectDeviceFn, sdrplay_api_SelectDevice);
SAME_FUNCTION(ReleaseDeviceFn, sdrplay_api_ReleaseDevice);
SAME_FUNCTION(GetErrorStringFn, sdrplay_api_GetErrorString);
SAME_FUNCTION(GetLastErrorFn, sdrplay_api_GetLastError);
SAME_FUNCTION(GetDeviceParamsFn, sdrplay_api_GetDeviceParams);
SAME_FUNCTION(InitFn, sdrplay_api_Init);
SAME_FUNCTION(UninitFn, sdrplay_api_Uninit);
SAME_FUNCTION(UpdateFn, sdrplay_api_Update);
static_assert(shape::same_prototype<r::StreamCallback, sdrplay_api_StreamCallback_t>(),
              "the stream callback differs from sdrplay_api_StreamCallback_t");
static_assert(shape::same_prototype<r::EventCallback, sdrplay_api_EventCallback_t>(),
              "the event callback differs from sdrplay_api_EventCallback_t");
