#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "Catalog.hpp"
#include "Digest.hpp"
#include "EngineFixtures.hpp"
#include "ExchangeFixtures.hpp"
#include "HourTime.hpp"
#include "Http.hpp"
#include "HttpClient.hpp"
#include "HttpServer.hpp"
#include "Json.hpp"
#include "MiniJson.hpp"
#include "PageEdge.hpp"
#include "ResultsApi.hpp"
#include "ResultsStore.hpp"
#include "Sha256.hpp"

using namespace enginefix;
using namespace exfix;
using namespace httpfix;
using namespace std::chrono_literals;
using recorder::AcceleratorPriceRow;
using recorder::BatchRecord;
using recorder::FillRow;
using recorder::LiveChanges;
using recorder::LiveState;
using recorder::OpenOrderRow;
using recorder::OrderRow;
using recorder::OrderTerms;
using recorder::RowSide;

TEST(Http, ReadsOnlyWholeRequestsOfTheShapeItServesAndWritesJsonThatParses) {
    using namespace api;
    // Requests go to the edge of a mapped page, so that a parser that reads past the end faults.
    const auto parse = [](const std::string& text, Request& out) {
        const std::string_view view = text.size() <= 4096 ? std::string_view(atEdgeOfPage(text.data(), text.size()), text.size())
                                                          : std::string_view(text);
        return parseRequest(view, out);
    };
    using Query = std::vector<std::pair<std::string, std::string>>;

    {
        SCOPED_TRACE("requests that are read");
        struct Good {
            std::string text;
            std::string method;
            std::vector<std::string> segments;
            Query query;
        };
        const Good good[] = {
            { "GET / HTTP/1.1\r\n\r\n", "GET", {}, {} },
            { "GET /status HTTP/1.0\r\n\r\n", "GET", { "status" }, {} },
            { "GET /batches/ HTTP/1.1\r\nHost: x\r\n\r\n", "GET", { "batches" }, {} },
            { "GET /batches/12/orders/bid/0?limit=5&offset=10 HTTP/1.1\r\nHost: a\r\nAccept: */*\r\n\r\n",
              "GET", { "batches", "12", "orders", "bid", "0" }, { { "limit", "5" }, { "offset", "10" } } },
            { "GET /a%20b/c%2Fd?x=1+2&y=%41%62&z&&w= HTTP/1.1\r\n\r\n", "GET", { "a b", "c/d" },
              { { "x", "1 2" }, { "y", "Ab" }, { "z", "" }, { "w", "" } } },
            { "GET //a// HTTP/1.1\r\n\r\n", "GET", { "", "a", "" }, {} },
            { "POST /x HTTP/1.1\r\nContent-Length: 4\r\n\r\nbody", "POST", { "x" }, {} },
            { "GET /x HTTP/1.1\r\nEmpty:\r\nX-Tab:\tvalue\r\n\r\n", "GET", { "x" }, {} },
        };
        for (const Good& g : good) {
            Request request;
            ASSERT_EQ(parse(g.text, request), ParseStatus::OK) << g.text;
            EXPECT_EQ(request.method, g.method) << g.text;
            EXPECT_EQ(request.segments, g.segments) << g.text;
            EXPECT_EQ(request.query, g.query) << g.text;
        }

        const std::string whole = "GET /batches?limit=3 HTTP/1.1\r\nHost: x\r\n\r\n";
        for (std::size_t length = 0; length < whole.size(); ++length) {
            Request request;
            EXPECT_EQ(parse(whole.substr(0, length), request), ParseStatus::INCOMPLETE) << "cut to " << length;
        }
        Request request;
        EXPECT_EQ(parse("GET / HTTP/1.1\nHost: x\n\n", request), ParseStatus::INCOMPLETE) << "only CRLF ends a line";
    }

    {
        SCOPED_TRACE("requests that are refused");
        struct Bad {
            std::string text;
            ParseStatus status;
        };
        const Bad bad[] = {
            { "GET /status HTTP/1.1\r\nHost x\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /status\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET  /status HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "get /status HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET status HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET http://x/status HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET * HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a#b HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a\x01 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a\xC3\xA9 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a%zz HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a% HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a%4 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a%00 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a?%00=1 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET /a?=1 HTTP/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/2.0\r\n\r\n", ParseStatus::VERSION_NOT_SUPPORTED },
            { "GET / HTTP/1.2\r\n\r\n", ParseStatus::VERSION_NOT_SUPPORTED },
            { "GET / HTTP/1.1x\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / http/1.1\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/1.1 more\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/1.1\r\n folded: x\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/1.1\r\n: x\r\n\r\n", ParseStatus::BAD_REQUEST },
            { "GET / HTTP/1.1\r\nA: b\x01\r\n\r\n", ParseStatus::BAD_REQUEST },
        };
        for (const Bad& b : bad) {
            Request request;
            request.method = "KEPT";
            request.segments = { "kept" };
            EXPECT_EQ(parse(b.text, request), b.status) << b.text;
            EXPECT_TRUE(request.method == "KEPT" && request.segments == std::vector<std::string>{ "kept" } && request.query.empty())
                << "a refused request leaves the output as it was";
        }
    }

    {
        SCOPED_TRACE("the size of a request");
        const auto withHeader = [](std::size_t filler) { return "GET / HTTP/1.1\r\nX: " + std::string(filler, 'a') + "\r\n\r\n"; };
        Request request;
        EXPECT_EQ(parse(withHeader(MAX_REQUEST_BYTES - 23), request), ParseStatus::OK) << "exactly the limit";
        EXPECT_EQ(parse(withHeader(MAX_REQUEST_BYTES - 22), request), ParseStatus::HEADER_TOO_LARGE);
        EXPECT_EQ(parse(std::string(MAX_REQUEST_BYTES - 1, 'a'), request), ParseStatus::INCOMPLETE);
        EXPECT_EQ(parse(std::string(MAX_REQUEST_BYTES, 'a'), request), ParseStatus::HEADER_TOO_LARGE) << "no end in sight";
    }

    {
        SCOPED_TRACE("percent-decoding");
        const struct {
            std::string in;
            bool plus;
            bool ok;
            std::string out;
        } cases[] = {
            { "", false, true, "" },         { "abc", false, true, "abc" },      { "a%20b", false, true, "a b" },
            { "a+b", false, true, "a+b" },   { "a+b", true, true, "a b" },       { "%2B", true, true, "+" },
            { "%e9", false, true, "\xE9" },  { "%41%62", false, true, "Ab" },    { "%", false, false, "" },
            { "%1", false, false, "" },      { "%g1", false, false, "" },        { "%1g", false, false, "" },
            { "%00", false, false, "" },     { "a%", true, false, "" },
        };
        for (const auto& c : cases) {
            std::string out = "unchanged";
            EXPECT_EQ(percentDecode(c.in, out, c.plus), c.ok) << c.in;
            if (c.ok) {
                EXPECT_EQ(out, c.out) << c.in;
            }
        }
    }

    {
        SCOPED_TRACE("no input makes the parser read past what it was given");
        std::mt19937_64 rng(20261009);
        const std::string seeds[] = { "GET /batches/5/orders/bid/0?limit=5&offset=10 HTTP/1.1\r\nHost: a\r\nAccept: */*\r\n\r\n",
                                      "GET /a%20b?x=1+2&y=%41 HTTP/1.0\r\n\r\n", "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nbody" };
        std::size_t read = 0;
        for (std::size_t i = 0; i < 40'000; ++i) {
            std::string text = seeds[rng() % 3];
            const std::size_t mutations = rng() % 4;
            for (std::size_t m = 0; m < mutations && !text.empty(); ++m) {
                const std::size_t at = rng() % text.size();
                switch (rng() % 3) {
                case 0: text[at] = static_cast<char>(rng()); break;
                case 1: text.erase(at, 1 + rng() % 3); break;
                default: text.insert(at, 1, "\r\n %?&=#/\x01\xFF"[rng() % 12]); break;
                }
            }
            if (rng() % 8 == 0) text = std::string(rng() % 200, static_cast<char>(rng()));
            Request request;
            const ParseStatus status = parse(text, request);
            ASSERT_NE(status, ParseStatus::HEADER_TOO_LARGE) << "these are all short";
            if (status == ParseStatus::OK) {
                ++read;
                EXPECT_FALSE(request.method.empty());
                for (const std::string& segment : request.segments) EXPECT_EQ(segment.find('\0'), std::string::npos);
            }
        }
        EXPECT_GT(read, 1000u) << "the mutations must leave requests that are read, too";
    }

    {
        SCOPED_TRACE("the answer");
        Response response;
        response.status = 404;
        response.body = "{}";
        EXPECT_EQ(render(response),
                  "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
        response.status = 405;
        response.allow = "GET";
        EXPECT_EQ(render(response),
                  "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: application/json\r\nContent-Length: 2\r\nAllow: GET\r\n"
                  "Connection: close\r\n\r\n{}");
        const Response error = errorResponse(400, "bad \"x\"\n");
        EXPECT_EQ(error.status, 400);
        EXPECT_EQ(error.body, R"({"error":"bad \"x\"\n"})");
        for (const int status : { 200, 400, 403, 404, 405, 408, 431, 500, 503, 505 }) {
            EXPECT_STRNE(statusText(status), "Unknown") << status;
        }
    }

    {
        SCOPED_TRACE("the JSON writer");
        JsonWriter json;
        json.beginObject();
        json.key("a").beginArray().number(1).number(-2).beginObject().key("b").null().endObject().endArray();
        json.key("c").boolean(true).key("d").string("x");
        json.key("e").beginArray().endArray().key("f").beginObject().endObject();
        json.endObject();
        EXPECT_EQ(json.text(), R"({"a":[1,-2,{"b":null}],"c":true,"d":"x","e":[],"f":{}})");

        JsonWriter numbers;
        numbers.beginArray()
            .number(std::numeric_limits<std::int64_t>::min())
            .number(std::numeric_limits<std::int64_t>::max())
            .number(std::numeric_limits<std::uint64_t>::max())
            .number(std::uint8_t{ 255 })
            .number(0)
            .id(std::numeric_limits<std::uint64_t>::max())
            .endArray();
        EXPECT_EQ(numbers.text(), R"([-9223372036854775808,9223372036854775807,18446744073709551615,255,0,"18446744073709551615"])");

        JsonWriter fixed;
        fixed.beginArray().fixed(150, 2).fixed(33, 2).fixed(5, 2).fixed(0, 2).fixed(123456, 2).fixed(7, 0).fixed(1234, 3).endArray();
        EXPECT_EQ(fixed.text(), "[1.50,0.33,0.05,0.00,1234.56,7,1.234]");
        EXPECT_TRUE(minijson::parse(fixed.text()));

        JsonWriter odd;
        odd.beginArray().string("a\"b\\c\n\t\x01\x7f\xe9").endArray();
        EXPECT_EQ(odd.text(), "[\"a\\\"b\\\\c\\n\\t\\u0001\\u007f\\u00e9\"]");
        for (int byte = 0; byte < 256; ++byte) {
            const std::string one(1, static_cast<char>(byte));
            JsonWriter alone;
            alone.beginArray().string(one).endArray();
            const std::optional<minijson::Value> parsed = minijson::parse(alone.text());
            ASSERT_TRUE(parsed) << "byte " << byte;
            EXPECT_EQ((*parsed)[0].str(), one) << "byte " << byte;
        }
        EXPECT_FALSE(minijson::parse(R"({"a":1,})")) << "the reader used to check the writer is strict";
        EXPECT_FALSE(minijson::parse("[1] x"));
    }

    {
        SCOPED_TRACE("the start of an hour");
        const struct {
            std::uint64_t hour;
            const char* text;
        } anchors[] = {
            { 0, "1970-01-01T00:00:00Z" },        { 262967, "1999-12-31T23:00:00Z" },  { 264384, "2000-02-29T00:00:00Z" },
            { 474768, "2024-02-29T00:00:00Z" },   { 474791, "2024-02-29T23:00:00Z" },  { 474792, "2024-03-01T00:00:00Z" },
            { 497449, "2026-10-01T01:00:00Z" },   { 497655, "2026-10-09T15:00:00Z" },  { 596523, "2038-01-19T03:00:00Z" },
            { 1140960, "2100-02-28T00:00:00Z" },  { 1140984, "2100-03-01T00:00:00Z" }, { 70389527, "9999-12-31T23:00:00Z" },
        };
        for (const auto& a : anchors) {
            ASSERT_TRUE(hourStart(a.hour)) << a.hour;
            EXPECT_EQ(*hourStart(a.hour), a.text) << a.hour;
        }
        EXPECT_FALSE(hourStart(70389528)) << "the first hour of the year 10000";
        EXPECT_FALSE(hourStart(std::numeric_limits<std::uint32_t>::max()));
        std::string previous;
        for (std::uint64_t hour = 0; hour < 200'000; ++hour) {
            const std::optional<std::string> text = hourStart(hour);
            ASSERT_TRUE(text);
            ASSERT_EQ(text->size(), 20u);
            ASSERT_GT(*text, previous) << "hours go forward, and the strings sort with them";
            ASSERT_EQ(std::stoi(text->substr(11, 2)), static_cast<int>(hour % 24));
            previous = *text;
        }
    }
}

namespace {

constexpr std::uint64_t BIGGEST_ID = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t HALF_ID = std::uint64_t{ 1 } << 63;

std::int64_t whole(std::uint64_t v) { return static_cast<std::int64_t>(v); }

recorder::Digest hashOfFile(const std::string& path) {
    const std::string bytes = slurp(path);
    recorder::Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish();
}

// The body as JSON, which it must be.
minijson::Value jsonChecked(const Reply& reply) {
    std::optional<minijson::Value> value = minijson::parse(reply.body);
    EXPECT_TRUE(value) << "not JSON: " << reply.body.substr(0, 300);
    return value ? *value : minijson::Value{};
}

std::string skuText(unsigned accelerator, unsigned interconnect, unsigned region) {
    return std::string(catalog::ACCELERATORS[accelerator].name) + "/" + catalog::INTERCONNECTS[interconnect] + "/" +
           catalog::REGIONS[region];
}

void expectHeader(const minijson::Value& j, const BatchRecord& r) {
    const recorder::BatchHeader& h = r.header;
    EXPECT_EQ(j.at("batch_id").str(), std::to_string(h.batchId));
    EXPECT_EQ(j.at("status").str(), h.status == recorder::BatchStatus::CLEARED ? "cleared" : "voided");
    if (h.status == recorder::BatchStatus::VOIDED) {
        EXPECT_EQ(j.at("void_reason").str(), toString(static_cast<VoidReason>(h.voidReason)));
    } else {
        EXPECT_TRUE(j.at("void_reason").isNull());
    }
    EXPECT_EQ(j.at("first_hour").integer(), whole(h.firstHour));
    EXPECT_EQ(j.at("first_hour_starts").str(), *api::hourStart(h.firstHour));
    EXPECT_EQ(j.at("event_seq").integer(), whole(h.eventSeq));
    EXPECT_EQ(j.at("bids").integer(), whole(h.bids));
    EXPECT_EQ(j.at("asks").integer(), whole(h.asks));
    EXPECT_EQ(j.at("arcs").integer(), whole(h.arcs));
    EXPECT_EQ(j.at("pivots").integer(), whole(h.pivots));
    EXPECT_EQ(j.at("units").integer(), whole(h.units));
    EXPECT_EQ(j.at("lifted_units").integer(), whole(h.liftedUnits));
    EXPECT_EQ(j.at("welfare").integer(), h.welfare);
    EXPECT_EQ(j.at("rank_sum").integer(), h.rankSum);
    EXPECT_EQ(j.at("fills").integer(), whole(h.fills));
    EXPECT_EQ(j.at("certified").isTrue(), h.certified != 0);
    EXPECT_EQ(j.at("solver_version").integer(), whole(h.solverVersion));
    EXPECT_EQ(j.at("digest").str(), hexOf(recorder::batchDigest(r)));
}

std::vector<std::string> namesIn(std::uint64_t mask, std::size_t count, const std::function<std::string(std::size_t)>& nameOf) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        if ((mask >> i) & 1) out.push_back(nameOf(i));
    }
    return out;
}

void expectNames(const minijson::Value& j, const std::vector<std::string>& want) {
    ASSERT_EQ(j.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) EXPECT_EQ(j[i].str(), want[i]);
}

// What an order asked for, as the API says it: the names of the models, interconnects and regions, the runs of
// consecutive hours, and the curve.
void expectTerms(const minijson::Value& j, const OrderTerms& t, HourIndex firstHour) {
    expectNames(j.at("accelerators"), namesIn(t.acceleratorMask, catalog::ACCELERATOR_COUNT, [](std::size_t i) { return std::string(catalog::ACCELERATORS[i].name); }));
    expectNames(j.at("interconnects"), namesIn(t.interconnectMask, catalog::INTERCONNECT_COUNT, [](std::size_t i) { return std::string(catalog::INTERCONNECTS[i]); }));
    expectNames(j.at("regions"), namesIn(t.regionMask, catalog::REGION_COUNT, [](std::size_t i) { return std::string(catalog::REGIONS[i]); }));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (((t.slotMask >> bit) & 1) == 0) continue;
        if (!runs.empty() && runs.back().second + 1 == std::uint64_t{ firstHour } + bit) runs.back().second = std::uint64_t{ firstHour } + bit;
        else runs.emplace_back(std::uint64_t{ firstHour } + bit, std::uint64_t{ firstHour } + bit);
    }
    const minijson::Value& hours = j.at("hours");
    ASSERT_EQ(hours.size(), runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
        EXPECT_EQ(hours[i].at("from").integer(), whole(runs[i].first));
        EXPECT_EQ(hours[i].at("from_starts").str(), *api::hourStart(runs[i].first));
        EXPECT_EQ(hours[i].at("to").integer(), whole(runs[i].second));
        EXPECT_EQ(hours[i].at("to_starts").str(), *api::hourStart(runs[i].second));
    }

    EXPECT_EQ(j.at("per_slot_cap").integer(), whole(t.perSlotCap));
    EXPECT_EQ(j.at("total_units").integer(), whole(t.totalUnits));
    const minijson::Value& curve = j.at("curve");
    ASSERT_EQ(curve.size(), t.stepCount);
    for (std::size_t k = 0; k < t.stepCount; ++k) {
        EXPECT_EQ(curve[k].at("quantity").integer(), whole(t.stepQuantity[k]));
        EXPECT_EQ(curve[k].at("price").integer(), whole(t.stepPrice[k]));
    }
}

void expectOrder(const minijson::Value& j, const OrderRow& r, HourIndex firstHour) {
    EXPECT_EQ(j.at("side").str(), r.side == RowSide::BID ? "bid" : "ask");
    EXPECT_EQ(j.at("rank").integer(), whole(r.rank));
    EXPECT_EQ(j.at("participant_id").integer(), whole(r.participant));
    EXPECT_EQ(j.at("order_token").str(), std::to_string(r.token));
    expectTerms(j, r.terms, firstHour);
    EXPECT_EQ(j.at("filled_units").integer(), whole(r.filled));
    EXPECT_EQ(j.at("unfilled_units").integer(), whole(r.unfilled()));
    EXPECT_EQ(j.at("lifted_units").integer(), whole(r.lifted));
    EXPECT_EQ(j.at("amount").integer(), r.amount);
}

void expectOpenOrder(const minijson::Value& j, const OpenOrderRow& r, HourIndex firstHour) {
    EXPECT_EQ(j.at("side").str(), r.side == RowSide::BID ? "bid" : "ask");
    EXPECT_EQ(j.at("rank").integer(), whole(r.rank));
    EXPECT_EQ(j.at("participant_id").integer(), whole(r.participant));
    EXPECT_EQ(j.at("order_token").str(), std::to_string(r.token));
    expectTerms(j, r.terms, firstHour);
    EXPECT_EQ(j.find("filled_units"), nullptr) << "an order that has not cleared has no result";
}

void expectItem(const minijson::Value& j, std::uint32_t hour, unsigned accelerator, unsigned interconnect, unsigned region) {
    EXPECT_EQ(j.at("hour").integer(), whole(hour));
    EXPECT_EQ(j.at("starts").str(), *api::hourStart(hour));
    EXPECT_EQ(j.at("accelerator").integer(), whole(accelerator));
    EXPECT_EQ(j.at("interconnect").integer(), whole(interconnect));
    EXPECT_EQ(j.at("region").integer(), whole(region));
    EXPECT_EQ(j.at("sku").str(), skuText(accelerator, interconnect, region));
}

void expectFill(const minijson::Value& j, const FillRow& r) {
    EXPECT_EQ(j.at("side").str(), r.side == RowSide::BID ? "bid" : "ask");
    EXPECT_EQ(j.at("rank").integer(), whole(r.rank));
    expectItem(j, r.hour, r.accelerator, r.interconnect, r.region);
    EXPECT_EQ(j.at("price").integer(), whole(r.price));
    EXPECT_EQ(j.at("units").integer(), whole(r.units));
    EXPECT_EQ(j.at("lifted").isTrue(), r.lifted != 0);
}

void expectPrice(const minijson::Value& j, const AcceleratorPriceRow& r) {
    EXPECT_EQ(j.at("hour").integer(), whole(r.hour));
    EXPECT_EQ(j.at("starts").str(), *api::hourStart(r.hour));
    EXPECT_EQ(j.at("accelerator").integer(), whole(r.accelerator));
    EXPECT_EQ(j.at("name").str(), catalog::ACCELERATORS[r.accelerator].name);
    EXPECT_EQ(j.at("units").integer(), whole(r.units));
    EXPECT_EQ(j.at("amount").integer(), r.amount);
    EXPECT_EQ(j.at("price_min").integer(), whole(r.priceMin));
    EXPECT_EQ(j.at("price_max").integer(), whole(r.priceMax));
    EXPECT_LE(r.priceMin, r.priceMax);

    // The average is the money over the units to a hundredth of a tick, so it is no more than half a hundredth
    // from the exact value.
    const minijson::Value& average = j.at("price_avg");
    ASSERT_EQ(average.kind, minijson::Value::Kind::NUMBER);
    const std::size_t point = average.text.find('.');
    ASSERT_NE(point, std::string::npos);
    ASSERT_EQ(average.text.size(), point + 3) << average.text << ": two decimals";
    const std::int64_t hundredths = std::stoll(average.text.substr(0, point)) * 100 + std::stoll(average.text.substr(point + 1));
    const std::int64_t off = hundredths * whole(r.units) - r.amount * 100;
    EXPECT_LE(std::abs(off), whole(r.units) / 2 + 1) << average.text << " for " << r.amount << " over " << r.units;
    EXPECT_GE(hundredths, whole(r.priceMin) * 100) << "an average is not below the cheapest item";
    EXPECT_LE(hundredths, whole(r.priceMax) * 100) << "or above the dearest";
}

struct Data {
    std::vector<BatchRecord> a; // recorder-1: every batch
    std::vector<BatchRecord> b; // recorder-2: behind, with one batch that differs and one that recorder-1 lacks
};

// Batch 0 is the design's example, batch 1 a crowd, batch 2 nothing, batch 3 a void, batch 5 a second crowd
// of the same participants; and copies of batch 0 under the largest ids, which are the ones that SQLite
// sorts below the others.
Data buildData() {
    const HourIndex first = static_cast<HourIndex>(schedule::firstSlotHour(schedule::Schedule{}, 0));
    Harness h{ EngineConfig{} };
    const auto at = [](Order o, HourIndex hour) {
        o.baseHour = hour;
        return o;
    };
    h.order(at(bid(101, 1, 0b1111111111, 1, { { 2, 2400 }, { 3, 2200 }, { 5, 2000 } }, A100 | H100), first));
    h.order(at(ask(201, 1, 0b1111, 4, { { 8, 1800 }, { 8, 2000 } }, H100), first));
    h.order(at(bid(102, 1, 0b11, 3, { { 6, 3000 } }, H200), first));
    h.order(at(ask(202, 1, 0b11, 3, { { 4, 700 }, { 4, 900 } }, H200), first));
    h.order(at(bid(103, 1, 0b1, 1, { { 1, 5 } }, A100), first));
    h.clearBatch(0);

    Lcg rng{ 20261009 };
    for (OrderToken t = 1; t <= 60; ++t) h.order(randomBid(rng, 1 + rng.below(6), t, first + 1));
    for (OrderToken t = 1; t <= 40; ++t) h.order(randomAsk(rng, 11 + rng.below(3), t, first + 1));
    h.clearBatch(1);
    h.clearBatch(2);
    for (OrderToken t = 1; t <= 20; ++t) h.order(randomBid(rng, 1 + rng.below(6), 100 + t, first + 3));
    for (OrderToken t = 1; t <= 10; ++t) h.order(randomAsk(rng, 11 + rng.below(3), 100 + t, first + 3));
    h.clearBatch(5);

    const std::vector<BatchRecord> day = recordsOf(h);
    Data d;
    if (day.size() != 4) return d;
    const auto renamed = [](BatchRecord r, BatchId id, std::uint64_t eventSeq) {
        r.header.batchId = id;
        r.header.eventSeq = eventSeq;
        return r;
    };
    BatchRecord voided;
    voided.header.batchId = 3;
    voided.header.status = recorder::BatchStatus::VOIDED;
    voided.header.voidReason = static_cast<std::uint8_t>(VoidReason::PIVOT_BUDGET);
    voided.header.firstHour = first + 3;
    voided.header.eventSeq = 500;
    voided.header.bids = 3;
    voided.header.asks = 2;
    voided.header.arcs = 61;
    voided.header.pivots = 5;
    voided.header.solverVersion = SOLVER_VERSION;
    BatchRecord altered = day[1];
    altered.fills.front().price += 1;
    altered.prices.front().priceMax += 1;
    d.a = { day[0], day[1], day[2], voided, day[3], renamed(day[0], HALF_ID, 1001), renamed(day[0], BIGGEST_ID, 1002) };
    d.b = { day[0], altered, day[2], renamed(day[3], 4, 700) };
    return d;
}

std::unique_ptr<api::HttpServer> startServer(std::vector<api::RecorderFile> files, bool openOrders) {
    api::HttpServer::Options options;
    options.port = 0;
    options.workers = 4;
    options.requestTimeout = 400ms;
    api::ApiOptions apiOptions;
    apiOptions.serveOpenOrders = openOrders;
    auto server = std::make_unique<api::HttpServer>(options, [files, apiOptions] {
        auto results = std::make_shared<api::ResultsApi>(files, apiOptions);
        return api::HttpServer::Handler([results](const api::Request& request) { return results->handle(request); });
    });
    server->start();
    return server;
}

class ApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = buildData();
        ASSERT_EQ(data_.a.size(), 7u) << "the engine did not make the batches that the test needs";
        ASSERT_GT(data_.a[1].fills.size(), 30u);
        ASSERT_GT(data_.a[1].orders.size(), 30u);

        // Recorder-1 has applied more of the log than recorder-2, so its open orders are the ones to serve.
        openFirst_ = static_cast<HourIndex>(schedule::firstSlotHour(schedule::Schedule{}, 9));
        const OrderTerms wide = makeTerms(H100 | H200, NVLINK, US_WEST | US_EAST, 0b11100, 2, { { 3, 900 }, { 2, 800 } });
        const OrderTerms gappy = makeTerms(A100, NVLINK | PCIE, EU_WEST, 0b1100101, 4, { { 4, 300 } });
        const OrderTerms sell = makeTerms(H200, NVLINK, EU_WEST, 0b1, 5, { { 5, 600 }, { 5, 700 } });
        liveA_.state = LiveState{ 9, openFirst_, 7000 };
        liveA_.upserts = { OpenOrderRow{ RowSide::BID, 0, 101, 4001, wide }, OpenOrderRow{ RowSide::BID, 1, 3, 4002, gappy },
                           OpenOrderRow{ RowSide::BID, 2, 101, 4003, gappy }, OpenOrderRow{ RowSide::ASK, 0, 201, 4004, sell } };
        liveB_.state = LiveState{ 9, openFirst_, 6000 };
        liveB_.upserts = { OpenOrderRow{ RowSide::BID, 0, 101, 9999, sell } };
        {
            recorder::ResultsStore a(fileA_.path());
            for (const BatchRecord& r : data_.a) a.write(r);
            a.applyLive(liveA_);
            recorder::ResultsStore b(fileB_.path());
            for (const BatchRecord& r : data_.b) b.write(r);
            b.applyLive(liveB_);
        }
        hashOfB_ = hashOfFile(fileB_.path());

        files_ = { { "recorder-1", fileA_.path() }, { "recorder-2", fileB_.path() }, { "recorder-3", fileC_.path() } };
        server_ = startServer(files_, false);
        port_ = server_->port();
        openServer_ = startServer(files_, true);
        openPort_ = openServer_->port();
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (openServer_) openServer_->stop();
    }

    Reply get(const std::string& target) const {
        return roundTrip(port_, "GET " + target + " HTTP/1.1\r\nHost: test\r\n\r\n");
    }

    Reply getOpen(const std::string& target) const {
        return roundTrip(openPort_, "GET " + target + " HTTP/1.1\r\nHost: test\r\n\r\n");
    }

    minijson::Value ok(const std::string& target) const {
        const Reply reply = get(target);
        EXPECT_EQ(reply.status, 200) << target << ": " << reply.body.substr(0, 300);
        return jsonChecked(reply);
    }

    minijson::Value okOpen(const std::string& target) const {
        const Reply reply = getOpen(target);
        EXPECT_EQ(reply.status, 200) << target << ": " << reply.body.substr(0, 300);
        return jsonChecked(reply);
    }

    // Who has each batch id, largest first, and the record that a client is given for it: the first recorder's.
    struct Holding {
        const BatchRecord* record{ nullptr };
        std::vector<std::string> heldBy;
        std::vector<recorder::Digest> digests;
    };
    std::map<BatchId, Holding, std::greater<>> holdings() const {
        std::map<BatchId, Holding, std::greater<>> out;
        const std::pair<const char*, const std::vector<BatchRecord>*> recorders[] = { { "recorder-1", &data_.a },
                                                                                       { "recorder-2", &data_.b } };
        for (const auto& [name, records] : recorders) {
            for (const BatchRecord& r : *records) {
                Holding& h = out[r.header.batchId];
                if (h.record == nullptr) h.record = &r;
                h.heldBy.push_back(name);
                h.digests.push_back(recorder::batchDigest(r));
            }
        }
        return out;
    }

    Data data_;
    TempFile fileA_, fileB_, fileC_;
    recorder::Digest hashOfB_{};
    HourIndex openFirst_{ 0 };
    LiveChanges liveA_, liveB_;
    std::vector<api::RecorderFile> files_;
    std::unique_ptr<api::HttpServer> server_;
    std::unique_ptr<api::HttpServer> openServer_;
    std::uint16_t port_{ 0 };
    std::uint16_t openPort_{ 0 };
};

} // namespace

TEST_F(ApiTest, AnswersEveryEndpointFromTheFilesItIsGiven) {
    const auto held = holdings();
    std::vector<BatchId> ids;
    for (const auto& [id, h] : held) ids.push_back(id);
    ASSERT_EQ(ids.size(), 8u);
    ASSERT_EQ(ids.front(), BIGGEST_ID) << "the largest ids come first, as unsigned numbers";
    ASSERT_EQ(ids[1], HALF_ID);

    {
        SCOPED_TRACE("the root and the status");
        const minijson::Value root = ok("/");
        EXPECT_TRUE(root.at("read_only").isTrue());
        EXPECT_EQ(root.at("endpoints").size(), 9u);
        EXPECT_EQ(okOpen("/").at("endpoints").size(), 10u) << "the open orders are listed when they are served";

        const minijson::Value status = ok("/status");
        EXPECT_TRUE(status.at("ok").isTrue());
        const minijson::Value& marks = status.at("watermarks");
        ASSERT_EQ(marks.size(), 3u);
        const std::pair<const char*, const std::string*> files[] = { { "recorder-1", &fileA_.path() }, { "recorder-2", &fileB_.path() } };
        for (std::size_t i = 0; i < 2; ++i) {
            const recorder::ResultsStore::Watermark w =
                recorder::ResultsStore(*files[i].second, recorder::ResultsStore::Mode::READ_ONLY).watermark();
            EXPECT_EQ(marks[i].at("recorder").str(), files[i].first);
            EXPECT_TRUE(marks[i].at("readable").isTrue());
            EXPECT_EQ(marks[i].at("batches").integer(), whole(w.batches));
            EXPECT_EQ(marks[i].at("last_batch_id").str(), std::to_string(w.lastBatchId));
            EXPECT_EQ(marks[i].at("last_event_seq").integer(), whole(w.lastEventSeq));
        }
        EXPECT_EQ(marks[0].at("batches").integer(), 7);
        EXPECT_EQ(marks[0].at("last_batch_id").str(), std::to_string(BIGGEST_ID)) << "the one with the highest event sequence number";
        EXPECT_EQ(marks[1].at("batches").integer(), 4);
        EXPECT_FALSE(marks[2].at("readable").isTrue()) << "a file that is not there yet";
        EXPECT_EQ(marks[2].find("batches"), nullptr);
    }

    {
        SCOPED_TRACE("the list of batches, and its pages");
        const minijson::Value all = ok("/batches");
        ASSERT_EQ(all.at("batches").size(), ids.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const Holding& h = held.at(ids[i]);
            const minijson::Value& item = all.at("batches")[i];
            expectHeader(item.at("batch"), *h.record);
            ASSERT_EQ(item.at("held_by").size(), h.heldBy.size());
            for (std::size_t k = 0; k < h.heldBy.size(); ++k) EXPECT_EQ(item.at("held_by")[k].str(), h.heldBy[k]);
            const bool agree = std::all_of(h.digests.begin(), h.digests.end(), [&](const recorder::Digest& d) { return d == h.digests.front(); });
            EXPECT_EQ(item.at("agree").isTrue(), agree) << "batch " << ids[i];
        }
        EXPECT_TRUE(all.at("next_before").isNull());

        std::vector<std::string> seen;
        std::string before;
        for (int page = 0; page < 10; ++page) {
            const minijson::Value p = ok("/batches?limit=3" + (before.empty() ? std::string() : "&before=" + before));
            for (const minijson::Value& item : p.at("batches").items) seen.push_back(item.at("batch").at("batch_id").str());
            if (p.at("next_before").isNull()) break;
            before = p.at("next_before").str();
        }
        std::vector<std::string> want;
        for (const BatchId id : ids) want.push_back(std::to_string(id));
        EXPECT_EQ(seen, want) << "pages of three give every batch once, in order";

        EXPECT_EQ(ok("/batches?before=0").at("batches").size(), 0u);
        EXPECT_EQ(ok("/batches?before=9223372036854775808").at("batches")[0].at("batch").at("batch_id").str(), "5");
        EXPECT_EQ(ok("/batches?before=18446744073709551615&limit=1").at("batches")[0].at("batch").at("batch_id").str(), "9223372036854775808");
    }

    {
        SCOPED_TRACE("one batch, which recorders have it, and whether they agree");
        for (const BatchId id : ids) {
            SCOPED_TRACE("batch " + std::to_string(id));
            const Holding& h = held.at(id);
            const minijson::Value j = ok("/batches/" + std::to_string(id) + "?verify=1");
            expectHeader(j.at("batch"), *h.record);
            ASSERT_EQ(j.at("held_by").size(), h.heldBy.size());
            for (std::size_t k = 0; k < h.heldBy.size(); ++k) {
                EXPECT_EQ(j.at("held_by")[k].str(), h.heldBy[k]);
                EXPECT_EQ(j.at("digests").at(h.heldBy[k]).str(), hexOf(h.digests[k]));
                EXPECT_TRUE(j.at("intact").at(h.heldBy[k]).isTrue());
            }
        }
        EXPECT_FALSE(ok("/batches/1").at("agree").isTrue()) << "recorder-2 has another fill price in batch 1";
        EXPECT_TRUE(ok("/batches/0").at("agree").isTrue());
        EXPECT_TRUE(ok("/batches/2").at("agree").isTrue());
        EXPECT_EQ(ok("/batches/4").at("held_by")[0].str(), "recorder-2") << "a batch that only recorder-2 has";
        EXPECT_EQ(ok("/batches/4").at("held_by").size(), 1u);
        EXPECT_EQ(ok("/batches/3").at("batch").at("void_reason").str(), "PIVOT_BUDGET");
        EXPECT_EQ(ok("/batches/0").find("intact"), nullptr) << "only when asked";
    }

    const BatchRecord& crowd = data_.a[1];
    {
        SCOPED_TRACE("the orders of a batch, in pages");
        std::vector<minijson::Value> rows;
        std::size_t offset = 0;
        for (int page = 0; page < 40; ++page) {
            const minijson::Value p = ok("/batches/1/orders?limit=7&offset=" + std::to_string(offset));
            EXPECT_EQ(p.at("recorder").str(), "recorder-1");
            EXPECT_FALSE(p.at("agree").isTrue());
            EXPECT_EQ(p.at("total").integer(), whole(crowd.orders.size()));
            EXPECT_EQ(p.at("offset").integer(), whole(offset));
            EXPECT_EQ(p.at("limit").integer(), 7);
            for (const minijson::Value& item : p.at("orders").items) rows.push_back(item);
            if (p.at("next_offset").isNull()) break;
            offset = static_cast<std::size_t>(p.at("next_offset").integer());
        }
        ASSERT_EQ(rows.size(), crowd.orders.size());
        for (std::size_t i = 0; i < rows.size(); ++i) expectOrder(rows[i], crowd.orders[i], crowd.header.firstHour);

        const minijson::Value past = ok("/batches/1/orders?offset=100000");
        EXPECT_EQ(past.at("orders").size(), 0u);
        EXPECT_TRUE(past.at("next_offset").isNull());
        EXPECT_EQ(ok("/batches/1/orders").at("orders").size(), std::min<std::size_t>(crowd.orders.size(), 100)) << "the default limit";
    }

    {
        SCOPED_TRACE("one order, with its fills");
        const auto check = [&](const OrderRow& row) {
            SCOPED_TRACE(std::string(row.side == RowSide::BID ? "bid " : "ask ") + std::to_string(row.rank));
            const minijson::Value j = ok(std::string("/batches/1/orders/") + (row.side == RowSide::BID ? "bid/" : "ask/") + std::to_string(row.rank));
            expectOrder(j.at("order"), row, crowd.header.firstHour);
            std::vector<FillRow> want;
            for (const FillRow& f : crowd.fills) {
                if (f.side == row.side && f.rank == row.rank) want.push_back(f);
            }
            ASSERT_EQ(j.at("fills").size(), want.size());
            for (std::size_t i = 0; i < want.size(); ++i) expectFill(j.at("fills")[i], want[i]);
        };
        const auto firstAsk = std::find_if(crowd.orders.begin(), crowd.orders.end(), [](const OrderRow& r) { return r.side == RowSide::ASK; });
        const auto bigBid = std::find_if(crowd.orders.begin(), crowd.orders.end(), [](const OrderRow& r) { return r.side == RowSide::BID && r.filled > 1; });
        ASSERT_NE(firstAsk, crowd.orders.end());
        ASSERT_NE(bigBid, crowd.orders.end());
        check(crowd.orders.front());
        check(*firstAsk);
        check(*bigBid);
        check(crowd.orders.back());
    }

    {
        SCOPED_TRACE("the fills of a batch, in pages, and its prices");
        std::vector<minijson::Value> rows;
        std::size_t offset = 0;
        for (int page = 0; page < 40; ++page) {
            const minijson::Value p = ok("/batches/1/fills?limit=13&offset=" + std::to_string(offset));
            EXPECT_EQ(p.at("total").integer(), whole(crowd.header.fills));
            for (const minijson::Value& item : p.at("fills").items) rows.push_back(item);
            if (p.at("next_offset").isNull()) break;
            offset = static_cast<std::size_t>(p.at("next_offset").integer());
        }
        ASSERT_EQ(rows.size(), crowd.fills.size());
        for (std::size_t i = 0; i < rows.size(); ++i) expectFill(rows[i], crowd.fills[i]);

        const minijson::Value prices = ok("/batches/1/prices");
        ASSERT_EQ(prices.at("prices").size(), crowd.prices.size());
        for (std::size_t i = 0; i < crowd.prices.size(); ++i) expectPrice(prices.at("prices")[i], crowd.prices[i]);
    }

    {
        SCOPED_TRACE("a void, an empty batch, and the largest id");
        for (const char* id : { "3", "2" }) {
            EXPECT_EQ(ok(std::string("/batches/") + id + "/orders").at("total").integer(), 0);
            EXPECT_EQ(ok(std::string("/batches/") + id + "/orders").at("orders").size(), 0u);
            EXPECT_EQ(ok(std::string("/batches/") + id + "/fills").at("fills").size(), 0u);
            EXPECT_EQ(ok(std::string("/batches/") + id + "/prices").at("prices").size(), 0u);
        }
        const BatchRecord& example = data_.a[0];
        const minijson::Value big = ok("/batches/18446744073709551615/orders");
        EXPECT_EQ(big.at("batch_id").str(), "18446744073709551615");
        ASSERT_EQ(big.at("orders").size(), example.orders.size());
        for (std::size_t i = 0; i < example.orders.size(); ++i) expectOrder(big.at("orders")[i], example.orders[i], example.header.firstHour);
    }

    {
        SCOPED_TRACE("a participant's orders across batches, newest first");
        struct Row {
            BatchId batch;
            HourIndex firstHour;
            OrderRow row;
        };
        const auto expected = [&](std::uint32_t participant, std::optional<OrderToken> token) {
            std::vector<Row> rows;
            for (const auto& [id, h] : held) {
                for (const OrderRow& r : h.record->orders) {
                    if (r.participant == participant && (!token || r.token == *token)) rows.push_back(Row{ id, h.record->header.firstHour, r });
                }
            }
            std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
                return std::make_tuple(x.batch, x.row.side, x.row.rank) > std::make_tuple(y.batch, y.row.side, y.row.rank);
            });
            return rows;
        };
        const auto cursorOf = [](const Row& r) {
            return std::to_string(r.batch) + (r.row.side == RowSide::BID ? ".bid." : ".ask.") + std::to_string(r.row.rank);
        };
        const auto pagesOf = [&](const std::string& path, std::size_t limit) {
            std::vector<minijson::Value> rows;
            std::string cursor;
            for (int page = 0; page < 200; ++page) {
                const minijson::Value p = ok(path + (path.find('?') == std::string::npos ? "?" : "&") + "limit=" + std::to_string(limit) +
                                             (cursor.empty() ? std::string() : "&cursor=" + cursor));
                for (const minijson::Value& item : p.at("orders").items) rows.push_back(item);
                if (p.at("next_cursor").isNull()) break;
                cursor = p.at("next_cursor").str();
            }
            return rows;
        };

        for (const std::uint32_t participant : { 3u, 1u, 12u, 101u, 201u }) {
            SCOPED_TRACE("participant " + std::to_string(participant));
            const std::vector<Row> want = expected(participant, std::nullopt);
            ASSERT_FALSE(want.empty());
            for (const std::size_t limit : { std::size_t{ 1000 }, std::size_t{ 4 }, std::size_t{ 1 } }) {
                const std::vector<minijson::Value> got = pagesOf("/participants/" + std::to_string(participant) + "/orders", limit);
                ASSERT_EQ(got.size(), want.size()) << "pages of " << limit;
                for (std::size_t i = 0; i < want.size(); ++i) {
                    EXPECT_EQ(got[i].at("batch_id").str(), std::to_string(want[i].batch));
                    EXPECT_EQ(got[i].at("first_hour").integer(), whole(want[i].firstHour)) << "the hour its hours are counted from";
                    expectOrder(got[i], want[i].row, want[i].firstHour);
                }
            }
            const minijson::Value first = ok("/participants/" + std::to_string(participant) + "/orders?limit=2");
            EXPECT_EQ(first.at("participant_id").integer(), whole(participant));
            if (want.size() > 2) {
                EXPECT_EQ(first.at("next_cursor").str(), cursorOf(want[1])) << "the position of the last order on the page";
            }
        }

        const std::vector<Row> ofOne = expected(101, std::nullopt);
        ASSERT_GE(ofOne.size(), 3u);
        EXPECT_EQ(ofOne[0].batch, BIGGEST_ID);
        EXPECT_EQ(ofOne[1].batch, HALF_ID);
        const OrderToken token = ofOne.back().row.token;
        const std::vector<Row> withToken = expected(101, token);
        const std::vector<minijson::Value> got = pagesOf("/participants/101/orders?token=" + std::to_string(token), 2);
        ASSERT_EQ(got.size(), withToken.size());
        for (std::size_t i = 0; i < got.size(); ++i) expectOrder(got[i], withToken[i].row, withToken[i].firstHour);

        const minijson::Value nobody = ok("/participants/999999/orders");
        EXPECT_EQ(nobody.at("orders").size(), 0u);
        EXPECT_TRUE(nobody.at("next_cursor").isNull());
    }

    {
        SCOPED_TRACE("a model's price over the batches, in an hour and at a fixed lead");
        struct Want {
            BatchId batch;
            AcceleratorPriceRow row;
            std::vector<std::string> heldBy;
            bool agree;
        };
        const std::pair<const char*, const std::vector<BatchRecord>*> recorders[] = { { "recorder-1", &data_.a }, { "recorder-2", &data_.b } };
        const auto wanted = [&](std::uint8_t model, const std::function<HourIndex(const BatchRecord&)>& hourIn) {
            std::vector<Want> out;
            for (const auto& [id, h] : held) {
                std::vector<AcceleratorPriceRow> rows;
                std::vector<std::string> by;
                for (const auto& [name, records] : recorders) {
                    for (const BatchRecord& r : *records) {
                        if (r.header.batchId != id) continue;
                        for (const AcceleratorPriceRow& p : r.prices) {
                            if (p.accelerator == model && p.hour == hourIn(r)) {
                                rows.push_back(p);
                                by.push_back(name);
                            }
                        }
                    }
                }
                if (rows.empty()) continue;
                out.push_back(Want{ id, rows.front(), by, std::all_of(rows.begin(), rows.end(), [&](const AcceleratorPriceRow& p) { return p == rows.front(); }) });
            }
            return out;
        };
        const auto pages = [&](const std::string& path, std::size_t limit) {
            std::vector<minijson::Value> rows;
            std::string before;
            for (int page = 0; page < 50; ++page) {
                const minijson::Value p = ok(path + "&limit=" + std::to_string(limit) + (before.empty() ? std::string() : "&before=" + before));
                for (const minijson::Value& item : p.at("prices").items) rows.push_back(item);
                if (p.at("next_before").isNull()) break;
                before = p.at("next_before").str();
            }
            return rows;
        };
        const auto check = [&](const std::string& path, const std::vector<Want>& want) {
            for (const std::size_t limit : { std::size_t{ 100 }, std::size_t{ 2 }, std::size_t{ 1 } }) {
                const std::vector<minijson::Value> got = pages(path, limit);
                ASSERT_EQ(got.size(), want.size()) << path << " in pages of " << limit;
                for (std::size_t i = 0; i < want.size(); ++i) {
                    EXPECT_EQ(got[i].at("batch_id").str(), std::to_string(want[i].batch));
                    ASSERT_EQ(got[i].at("held_by").size(), want[i].heldBy.size());
                    for (std::size_t k = 0; k < want[i].heldBy.size(); ++k) EXPECT_EQ(got[i].at("held_by")[k].str(), want[i].heldBy[k]);
                    EXPECT_EQ(got[i].at("agree").isTrue(), want[i].agree) << "batch " << want[i].batch;
                    expectPrice(got[i], want[i].row);
                }
            }
        };

        const AcceleratorPriceRow probe = data_.a[0].prices.front();
        const HourIndex probeFirstHour = data_.a[0].header.firstHour;
        const std::string name = catalog::ACCELERATORS[probe.accelerator].name;
        const std::vector<Want> inAnHour = wanted(probe.accelerator, [&](const BatchRecord&) { return probe.hour; });
        ASSERT_GE(inAnHour.size(), 3u) << "the example and its two copies under the largest ids";
        EXPECT_EQ(inAnHour[0].batch, BIGGEST_ID);
        EXPECT_EQ(inAnHour[1].batch, HALF_ID);
        check("/prices?accelerator=" + name + "&hour=" + std::to_string(probe.hour), inAnHour);
        check("/prices?accelerator=" + std::to_string(probe.accelerator) + "&hour=" + std::to_string(probe.hour), inAnHour);

        const std::uint32_t lead = probe.hour - probeFirstHour;
        const std::vector<Want> atALead = wanted(probe.accelerator, [&](const BatchRecord& r) { return r.header.firstHour + lead; });
        ASSERT_GE(atALead.size(), 3u);
        check("/prices?accelerator=" + name + "&lead=" + std::to_string(lead), atALead);
        const minijson::Value one = ok("/prices?accelerator=" + name + "&lead=" + std::to_string(lead));
        EXPECT_EQ(one.at("lead").integer(), whole(lead));
        EXPECT_EQ(one.at("name").str(), name);
        EXPECT_EQ(one.find("hour"), nullptr);

        const AcceleratorPriceRow differing = data_.b[1].prices.front();
        const std::vector<Want> where = wanted(differing.accelerator, [&](const BatchRecord&) { return differing.hour; });
        check("/prices?accelerator=" + std::to_string(differing.accelerator) + "&hour=" + std::to_string(differing.hour), where);
        const auto one1 = std::find_if(where.begin(), where.end(), [](const Want& w) { return w.batch == 1; });
        ASSERT_NE(one1, where.end());
        EXPECT_FALSE(one1->agree) << "recorder-2 has another price for this model in batch 1";
        EXPECT_TRUE(wanted(probe.accelerator, [&](const BatchRecord&) { return probe.hour + 100; }).empty());
        EXPECT_EQ(ok("/prices?accelerator=" + name + "&hour=" + std::to_string(probe.hour + 100)).at("prices").size(), 0u);

        const minijson::Value batchPrices = ok("/batches/0/prices");
        ASSERT_EQ(batchPrices.at("prices").size(), data_.a[0].prices.size());
        for (std::size_t i = 0; i < data_.a[0].prices.size(); ++i) expectPrice(batchPrices.at("prices")[i], data_.a[0].prices[i]);
    }

    {
        SCOPED_TRACE("the open orders are served only to an API that was started to serve them");
        const Reply refused = get("/participants/101/open_orders");
        EXPECT_EQ(refused.status, 403);
        EXPECT_NE(refused.head.find("403 Forbidden"), std::string::npos);
        EXPECT_NE(jsonChecked(refused).at("error").str().find("--open-orders"), std::string::npos);
        EXPECT_EQ(refused.body.find("4001"), std::string::npos) << "and no order is in what it says";

        const auto openRowsOf = [](const LiveChanges& live, std::uint32_t participant) {
            std::vector<OpenOrderRow> rows;
            for (const OpenOrderRow& r : live.upserts) {
                if (r.participant == participant) rows.push_back(r);
            }
            std::sort(rows.begin(), rows.end(), [](const OpenOrderRow& x, const OpenOrderRow& y) { return std::make_pair(x.side, x.rank) < std::make_pair(y.side, y.rank); });
            return rows;
        };
        const auto expectOpen = [&](const std::string& target, const LiveChanges& live, std::uint32_t participant, const char* recorder) {
            const minijson::Value j = okOpen(target);
            EXPECT_EQ(j.at("participant_id").integer(), whole(participant));
            const minijson::Value& batch = j.at("open_batch");
            EXPECT_EQ(batch.at("batch_id").str(), std::to_string(live.state.batchId));
            EXPECT_EQ(batch.at("first_hour").integer(), whole(live.state.firstHour));
            EXPECT_EQ(batch.at("first_hour_starts").str(), *api::hourStart(live.state.firstHour));
            EXPECT_EQ(batch.at("event_seq").integer(), whole(live.state.eventSeq));
            EXPECT_EQ(batch.at("recorder").str(), recorder);
            const std::vector<OpenOrderRow> want = openRowsOf(live, participant);
            ASSERT_EQ(j.at("orders").size(), want.size());
            for (std::size_t i = 0; i < want.size(); ++i) expectOpenOrder(j.at("orders")[i], want[i], live.state.firstHour);
            EXPECT_EQ(j.at("watermarks").size(), 3u);
        };
        expectOpen("/participants/101/open_orders", liveA_, 101, "recorder-1");
        expectOpen("/participants/3/open_orders", liveA_, 3, "recorder-1");
        expectOpen("/participants/201/open_orders", liveA_, 201, "recorder-1");
        EXPECT_EQ(okOpen("/participants/101/open_orders").at("orders").size(), 2u);
        expectOpen("/participants/555/open_orders", liveA_, 555, "recorder-1");
        EXPECT_EQ(okOpen("/participants/555/open_orders").at("orders").size(), 0u) << "the open batch is there, and nothing of this participant's in it";

        SCOPED_TRACE("the recorder that has applied more events is the one that is served");
        LiveChanges newer;
        newer.state = LiveState{ 9, openFirst_, 8000 };
        newer.removals = { { RowSide::BID, 0 } };
        newer.upserts = { OpenOrderRow{ RowSide::BID, 1, 101, 5001, makeTerms(MI300X, PCIE, US_EAST, 0b1, 1, { { 1, 50 } }) } };
        {
            recorder::ResultsStore b(fileB_.path());
            b.applyLive(newer);
        }
        const minijson::Value j = okOpen("/participants/101/open_orders");
        EXPECT_EQ(j.at("open_batch").at("recorder").str(), "recorder-2");
        EXPECT_EQ(j.at("open_batch").at("event_seq").integer(), 8000);
        ASSERT_EQ(j.at("orders").size(), 1u) << "recorder-2 removed its first order and added one";
        expectOpenOrder(j.at("orders")[0], newer.upserts.front(), openFirst_);
        EXPECT_EQ(okOpen("/participants/3/open_orders").at("orders").size(), 0u) << "and has nothing of the other participant's";
    }
}

TEST_F(ApiTest, RefusesWhatItShouldAndWaitsAndSurvivesBadClients) {
    {
        SCOPED_TRACE("requests that cannot be served");
        const struct {
            const char* target;
            int status;
        } cases[] = {
            { "/nothing", 404 },
            { "/batches/1/nothing", 404 },
            { "/batches/1/orders/bid", 404 },
            { "/batches/1/orders/bid/0/x", 404 },
            { "/participants/1", 404 },
            { "/participants/1/fills", 404 },
            { "/batches/x", 400 },
            { "/batches/1?bogus=1", 400 },
            { "/batches/1/orders?limit=0", 400 },
            { "/batches/1/orders?limit=1001", 400 },
            { "/batches/1/orders?limit=abc", 400 },
            { "/batches/1/orders?limit=1&limit=2", 400 },
            { "/batches/1/orders?offset=-1", 400 },
            { "/batches/1/orders?offset=1000000001", 400 },
            { "/batches/1?wait_ms=5001", 400 },
            { "/batches/1?verify=maybe", 400 },
            { "/batches/99", 404 },
            { "/batches/18446744073709551616", 400 },
            { "/batches/1/orders/buy/0", 400 },
            { "/batches/1/orders/bid/x", 400 },
            { "/batches/1/orders/bid/4294967296", 400 },
            { "/batches/1/orders/bid/9999", 404 },
            { "/batches/1/orders/ask/9999", 404 },
            { "/participants/x/orders", 400 },
            { "/participants/4294967296/orders", 400 },
            { "/participants/1/orders?cursor=bad", 400 },
            { "/participants/1/orders?cursor=1.buy.2", 400 },
            { "/participants/1/orders?cursor=1.bid.4294967296", 400 },
            { "/participants/1/orders?token=x", 400 },
            { "/status?x=1", 400 },
            { "/batches?before=x", 400 },
            { "/batches?limit=1&before=1&extra=2", 400 },
            { "/prices", 400 },
            { "/prices?accelerator=nope&hour=1", 400 },
            { "/prices?accelerator=5&hour=1", 400 },
            { "/prices?accelerator=1", 400 },
            { "/prices?accelerator=1&hour=1&lead=0", 400 },
            { "/prices?accelerator=1&lead=47", 400 },
            { "/prices?accelerator=1&hour=4294967296", 400 },
            { "/prices?accelerator=1&hour=1&limit=0", 400 },
            { "/prices?accelerator=1&hour=1&before=x", 400 },
            { "/prices?accelerator=1&hour=1&bogus=1", 400 },
            { "/prices?accelerator=1&accelerator=2&hour=1", 400 },
            { "/prices/1", 404 },
            { "/participants/1/open_orders", 403 },
            { "/participants/x/open_orders", 403 },
            { "/participants/1/open_orders/x", 404 },
        };
        for (const auto& c : cases) {
            const Reply reply = get(c.target);
            EXPECT_EQ(reply.status, c.status) << c.target << ": " << reply.body.substr(0, 200);
            EXPECT_NE(reply.head.find("Content-Type: application/json"), std::string::npos) << c.target;
            const minijson::Value body = jsonChecked(reply);
            EXPECT_FALSE(body.at("error").str().empty()) << c.target;
        }
        const struct {
            const char* target;
            int status;
        } served[] = {
            { "/participants/x/open_orders", 400 },
            { "/participants/4294967296/open_orders", 400 },
            { "/participants/1/open_orders?x=1", 400 },
            { "/participants/1/open_orders/x", 404 },
        };
        for (const auto& c : served) {
            const Reply reply = getOpen(c.target);
            EXPECT_EQ(reply.status, c.status) << c.target << ": " << reply.body.substr(0, 200);
            EXPECT_FALSE(jsonChecked(reply).at("error").str().empty()) << c.target;
        }
        EXPECT_EQ(jsonChecked(get("/prices?accelerator=%3Cb%3E&hour=1")).at("error").str().find('<'), std::string::npos)
            << "the model is never put into the answer as it came";
        const minijson::Value missing = jsonChecked(get("/batches/99"));
        EXPECT_EQ(missing.at("watermarks").size(), 3u) << "so that a client can see how far each recorder has got";
        EXPECT_EQ(get("/%62atches").status, 200) << "an escaped letter is the letter";
        EXPECT_EQ(jsonChecked(get("/batches/%3Cscript%3E")).at("error").str(), "the batch id must be a whole number of at most 20 digits, not ?script?")
            << "text from the client is never put into an answer as it came";
    }

    {
        SCOPED_TRACE("the wrong request");
        EXPECT_EQ(roundTrip(port_, "garbage\r\n\r\n").status, 400);
        EXPECT_EQ(roundTrip(port_, "GET / HTTP/2.0\r\n\r\n").status, 505);
        EXPECT_EQ(roundTrip(port_, "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n").status, 431);
        const Reply post = roundTrip(port_, "POST /status HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}");
        EXPECT_EQ(post.status, 405);
        EXPECT_NE(post.head.find("Allow: GET"), std::string::npos);
        EXPECT_EQ(roundTrip(port_, "HEAD /status HTTP/1.1\r\n\r\n").status, 405);
        EXPECT_EQ(roundTrip(port_, "DELETE /batches/0 HTTP/1.1\r\n\r\n").status, 405);
        EXPECT_EQ(roundTrip(port_, std::vector<std::string>{ "GET /sta", "tus HTTP/1.1\r\nHo", "st: x\r\n\r", "\n" }, false, 30ms).status, 200)
            << "a request that comes in pieces";
    }

    {
        SCOPED_TRACE("clients that stall, or leave");
        const auto before = std::chrono::steady_clock::now();
        const Reply stalled = roundTrip(port_, "GET /status HTTP/1.1\r\nHost: x\r\n");
        EXPECT_EQ(stalled.status, 408);
        EXPECT_GE(std::chrono::steady_clock::now() - before, 350ms);
        EXPECT_EQ(roundTrip(port_, "", true).status, 0) << "a client that sends nothing gets nothing";
        EXPECT_EQ(roundTrip(port_, "GET /sta", true).status, 0) << "nor one that leaves half way";
        EXPECT_EQ(get("/status").status, 200) << "and the server goes on";
    }

    {
        SCOPED_TRACE("waiting for a batch that is about to be written");
        std::thread writer([&] {
            std::this_thread::sleep_for(250ms);
            recorder::ResultsStore a(fileA_.path());
            BatchRecord r = data_.a[0];
            r.header.batchId = 7;
            r.header.eventSeq = 5000;
            a.write(r);
        });
        const auto start = std::chrono::steady_clock::now();
        const Reply late = get("/batches/7?wait_ms=3000");
        const auto took = std::chrono::steady_clock::now() - start;
        writer.join();
        EXPECT_EQ(late.status, 200) << late.body;
        EXPECT_GE(took, 200ms);
        EXPECT_LT(took, 2500ms);

        const auto startNever = std::chrono::steady_clock::now();
        EXPECT_EQ(get("/batches/8?wait_ms=300").status, 404);
        EXPECT_GE(std::chrono::steady_clock::now() - startNever, 280ms);
        const auto startNow = std::chrono::steady_clock::now();
        EXPECT_EQ(get("/batches/9").status, 404);
        EXPECT_LT(std::chrono::steady_clock::now() - startNow, 250ms) << "without asking, it does not wait";
    }

    {
        SCOPED_TRACE("a recorder whose file appears after the API started");
        EXPECT_FALSE(ok("/status").at("watermarks")[2].at("readable").isTrue());
        const std::unique_ptr<api::HttpServer> onlyC = startServer({ { "recorder-3", fileC_.path() } }, true);
        EXPECT_EQ(roundTrip(onlyC->port(), "GET /participants/1/open_orders HTTP/1.1\r\nHost: t\r\n\r\n").status, 503)
            << "when no file can be read there is nothing to say about the open batch";
        {
            recorder::ResultsStore c(fileC_.path());
            BatchRecord r = data_.a[2];
            r.header.batchId = 100;
            r.header.eventSeq = 9000;
            const HourIndex h = r.header.firstHour;
            r.prices = { AcceleratorPriceRow{ h, 0, 2, 3, 1, 2 }, AcceleratorPriceRow{ h, 1, 3, 1, 0, 1 },
                         AcceleratorPriceRow{ h, 2, 3, 2, 0, 1 }, AcceleratorPriceRow{ h, 3, 8, 1, 0, 1 },
                         AcceleratorPriceRow{ h, 4, 4'000'000'000u, 4'000'000'000ll * 1'048'575, 1'048'575, 1'048'575 } };
            c.write(r);
        }
        const minijson::Value j = ok("/batches/100");
        ASSERT_EQ(j.at("held_by").size(), 1u);
        EXPECT_EQ(j.at("held_by")[0].str(), "recorder-3");
        EXPECT_TRUE(ok("/status").at("watermarks")[2].at("readable").isTrue());
        EXPECT_EQ(ok("/batches?limit=1").at("batches")[0].at("batch").at("batch_id").str(), std::to_string(BIGGEST_ID));

        const minijson::Value rounded = ok("/batches/100/prices");
        ASSERT_EQ(rounded.at("prices").size(), 5u);
        const char* const averages[] = { "1.50", "0.33", "0.67", "0.13", "1048575.00" };
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_EQ(rounded.at("prices")[i].at("price_avg").text, averages[i]) << "a half goes up, and the largest sums do not overflow";
        }

        const Reply none = roundTrip(onlyC->port(), "GET /participants/1/open_orders HTTP/1.1\r\nHost: t\r\n\r\n");
        EXPECT_EQ(none.status, 200);
        EXPECT_TRUE(jsonChecked(none).at("open_batch").isNull()) << "a recorder that has not seen an order or a clear has no open batch to name";
        EXPECT_EQ(jsonChecked(none).at("orders").size(), 0u);
        onlyC->stop();
    }

    {
        SCOPED_TRACE("many clients at once");
        const std::string prices = get("/batches/1/prices").body;
        std::atomic<int> wrong{ 0 };
        std::atomic<int> answered{ 0 };
        std::vector<std::thread> clients;
        for (int c = 0; c < 8; ++c) {
            clients.emplace_back([&, c] {
                static const char* const targets[] = { "/status",
                                                       "/batches?limit=2",
                                                       "/batches/0",
                                                       "/batches/1/orders?limit=5",
                                                       "/batches/1/prices",
                                                       "/participants/3/orders?limit=3",
                                                       "/batches/0/orders/bid/0",
                                                       "/prices?accelerator=1&lead=0&limit=3",
                                                       "/participants/101/open_orders" };
                std::mt19937 rng(static_cast<unsigned>(c));
                for (int i = 0; i < 25; ++i) {
                    const std::string target = targets[rng() % 9];
                    const Reply reply = roundTrip(target.find("open_orders") == std::string::npos ? port_ : openPort_,
                                                  "GET " + target + " HTTP/1.1\r\nHost: t\r\n\r\n");
                    if (reply.status != 200 || !minijson::parse(reply.body)) ++wrong;
                    if (target == "/batches/1/prices" && reply.body != prices) ++wrong;
                    ++answered;
                }
            });
        }
        for (std::thread& client : clients) client.join();
        EXPECT_EQ(answered.load(), 200);
        EXPECT_EQ(wrong.load(), 0);
    }

    EXPECT_EQ(hashOfFile(fileB_.path()), hashOfB_) << "the API only reads";
}
