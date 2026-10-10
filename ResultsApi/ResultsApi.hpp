#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Http.hpp"

namespace api {

// One recorder's SQLite file, under the name that responses give it.
struct RecorderFile {
    std::string name;
    std::string path;
};

struct ApiOptions {
    // The orders of the open batch are bids and asks that have not cleared. With no authentication they would
    // be readable by anyone who can reach the API, so they are not served unless the operator asks.
    bool serveOpenOrders{ false };
};

// The read-only API over the files of one or more recorders. A batch is read from a recorder that has it,
// and when more than one has it, the answer says whether they agree. An instance keeps a read-only
// connection to each file, so it belongs to one thread.
class ResultsApi {
public:
    explicit ResultsApi(std::vector<RecorderFile> files, ApiOptions options = {});
    ~ResultsApi();
    ResultsApi(const ResultsApi&) = delete;
    ResultsApi& operator=(const ResultsApi&) = delete;

    Response handle(const Request& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace api
