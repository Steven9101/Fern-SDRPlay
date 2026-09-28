// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include <string>

#include "json.h"
#include "settings.h"
#include "test.h"

namespace {

fern::json::Value parse(const std::string& text) {
    fern::json::Value v;
    std::string error;
    if (!fern::json::parse(text, v, error))
        test::report(__FILE__, __LINE__, "bad test JSON: " + error);
    return v;
}

std::optional<fern::Failure> open_with(const std::string& settings, fern::OpenRequest& out,
                                       const std::string& band = "\"sample_rate\":2000000,\"center\":7100000") {
    return fern::parse_open(parse("{\"type\":\"open\",\"signal\":\"iq\"," + band + ",\"settings\":" + settings + "}"),
                            out);
}

std::string message(const std::optional<fern::Failure>& f) { return f ? f->message : std::string(); }

}  // namespace

TEST(open_takes_every_setting) {
    fern::OpenRequest r;
    REQUIRE(!open_with("{\"device\":\"serial:2305078C35\",\"antenna\":\"b\",\"gain\":\"manual\",\"lna_state\":4,"
                       "\"if_gain_reduction\":35,\"bias_tee\":true,\"rf_notch\":true,\"dab_notch\":true,"
                       "\"am_notch\":false,\"ppm\":-1.5,\"dc_correction\":false,\"iq_correction\":false,"
                       "\"if_mode\":\"zero\",\"bandwidth\":\"600\"}",
                       r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::serial);
    CHECK_EQ(r.settings.device.serial, std::string("2305078C35"));
    CHECK(r.settings.antenna == fern::Antenna::b);
    CHECK(r.settings.gain == fern::GainMode::manual);
    CHECK_EQ(*r.settings.lna_state, 4u);
    CHECK_EQ(*r.settings.if_gain_reduction, 35);
    CHECK(r.settings.bias_tee && r.settings.rf_notch && r.settings.dab_notch && !r.settings.am_notch);
    CHECK_EQ(r.settings.ppm, -1.5);
    CHECK(!r.settings.dc_correction && !r.settings.iq_correction);
    CHECK_EQ(r.settings.bandwidth_khz, 600);
    CHECK_EQ(r.sample_rate, 2000000u);
    CHECK_EQ(r.center, 7100000.0);

    REQUIRE(!open_with("{\"device\":\"index:1\"}", r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::index);
    CHECK_EQ(r.settings.device.index, 1u);
    REQUIRE(!open_with("{}", r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::only);
    CHECK(r.settings.gain == fern::GainMode::automatic);
}

TEST(open_refuses_what_an_rsp_cannot_do) {
    fern::OpenRequest r;
    CHECK_HAS(message(fern::parse_open(
                  parse("{\"type\":\"open\",\"signal\":\"real\",\"sample_rate\":2000000,\"center\":7e6}"), r)),
              "signal = iq");
    CHECK_HAS(message(open_with("{}", r, "\"sample_rate\":20000000,\"center\":7100000")), "62500 to 10660000");
    CHECK_HAS(message(open_with("{}", r, "\"sample_rate\":50000,\"center\":7100000")), "62500 to 10660000");
    CHECK_HAS(message(open_with("{}", r, "\"sample_rate\":2000000,\"center\":0")), "center must be");
    CHECK_HAS(message(open_with("{\"squelch\":1,\"gain\":\"auto\"}", r)), "unknown setting module.squelch");
    CHECK_HAS(message(open_with("{\"gain\":\"38\"}", r)), "auto, manual or agc");
    CHECK_HAS(message(open_with("{\"lna_state\":3}", r)), "module.gain = manual");
    CHECK_HAS(message(open_with("{\"gain\":\"agc\",\"if_gain_reduction\":30}", r)), "AGC");
    CHECK(!open_with("{\"gain\":\"agc\",\"lna_state\":3}", r));
    CHECK_HAS(message(open_with("{\"gain\":\"manual\",\"if_gain_reduction\":19}", r)), "20 (the most gain)");
    CHECK_HAS(message(open_with("{\"gain\":\"manual\",\"lna_state\":28}", r)), "lna_state");
    CHECK_HAS(message(open_with("{\"ppm\":2000}", r)), "module.ppm");
    CHECK_HAS(message(open_with("{\"antenna\":\"d\"}", r)), "module.antenna");
    CHECK_HAS(message(open_with("{\"device\":\"port:1-1\"}", r)), "serial:<serial>, index:<n>");
    CHECK_HAS(message(open_with("{\"device\":\"serial:\"}", r)), "serial:<serial>");
    CHECK_HAS(message(open_with("{\"hdr\":true}", r)), "HDR frequencies");
    CHECK(!open_with("{\"hdr\":true}", r, "\"sample_rate\":2000000,\"center\":475000"));
    CHECK_HAS(message(open_with("{\"if_mode\":\"low\"}", r, "\"sample_rate\":3000000,\"center\":7100000")),
              "if_mode = low");
    CHECK_HAS(message(open_with("{\"bandwidth\":\"5000\"}", r)), "wider than the band");
    CHECK_HAS(message(open_with("{\"bandwidth\":1536}", r)), "module.bandwidth");
}

TEST(set_takes_live_settings_only) {
    fern::LiveChange c;
    REQUIRE(!fern::parse_set(parse("{\"gain\":\"manual\",\"lna_state\":2,\"if_gain_reduction\":44,\"ppm\":0.5,"
                                   "\"bias_tee\":true,\"rf_notch\":false,\"dab_notch\":true,\"am_notch\":true,"
                                   "\"dc_correction\":true,\"iq_correction\":false}"),
                             c));
    CHECK(*c.gain == fern::GainMode::manual);
    CHECK_EQ(*c.lna_state, 2u);
    CHECK_EQ(*c.if_gain_reduction, 44);
    CHECK_EQ(*c.ppm, 0.5);
    CHECK(*c.bias_tee && !*c.rf_notch && *c.dab_notch && *c.am_notch && *c.dc_correction && !*c.iq_correction);
    fern::LiveChange d;
    CHECK_HAS(message(fern::parse_set(parse("{\"antenna\":\"b\"}"), d)), "restart the band");
    CHECK_HAS(message(fern::parse_set(parse("{\"hdr\":true}"), d)), "restart the band");
    CHECK_HAS(message(fern::parse_set(parse("{\"gain\":\"auto\",\"lna_state\":1}"), d)), "module.gain is auto");
    CHECK_HAS(message(fern::parse_set(parse("{\"volume\":1}"), d)), "unknown setting");
    CHECK(d.empty());
}

TEST(schema_lists_the_settings_in_order_with_valid_types) {
    const fern::json::Value schema = fern::settings_schema();
    std::string keys;
    for (const fern::json::Value& s : schema.items()) {
        keys += s.find("key")->as_string() + " ";
        const std::string type = s.find("type")->as_string();
        CHECK(type == "string" || type == "number" || type == "boolean" || type == "choice");
        CHECK(s.find("help") && s.find("help")->as_string().size() <= 400);
        CHECK(s.find("label") && s.find("label")->as_string().size() <= 64);
    }
    CHECK_EQ(keys, std::string("device antenna gain lna_state if_gain_reduction bias_tee rf_notch dab_notch "
                               "am_notch hdr ppm dc_correction iq_correction if_mode bandwidth "));
    const fern::json::Value d = fern::describe_module();
    CHECK_EQ(d.find("id")->as_string(), std::string("sdrplay"));
    CHECK_EQ(d.find("version")->as_string(), std::string(FERN_SDRPLAY_VERSION));
}
