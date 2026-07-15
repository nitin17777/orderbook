#pragma once

#include "orderbook/command.hpp"

#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>

namespace orderbook {

// EventLog — append-only binary log of Commands.
//
// Format: sequence of fixed-size Command structs written raw.
// No framing, no compression — simple and fast.
// This works because Command is a POD (plain old data) type.
//
// In production you'd add a header with magic bytes + version,
// and CRC32 per record for corruption detection.
// We document that as a known limitation rather than implement it now.

class EventLog {
public:
    // Open a log file for writing (appends if file exists)
    explicit EventLog(const std::string& path)
        : path_(path)
        , out_(path, std::ios::binary | std::ios::app)
    {
        if (!out_.is_open())
            throw std::runtime_error("EventLog: cannot open file: " + path);
    }

    // Append a command to the log — called before applying to the book
    void append(const Command& cmd) {
        out_.write(reinterpret_cast<const char*>(&cmd), sizeof(Command));
        if (!out_.good())
            throw std::runtime_error("EventLog: write failed");
    }

    // Flush to disk explicitly
    void flush() { out_.flush(); }

    // Read all commands back from a log file
    static std::vector<Command> read_all(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            throw std::runtime_error("EventLog: cannot open for reading: " + path);

        std::vector<Command> commands;
        Command cmd;
        while (in.read(reinterpret_cast<char*>(&cmd), sizeof(Command))) {
            commands.push_back(cmd);
        }

        if (!in.eof() && in.fail())
            throw std::runtime_error("EventLog: read error in: " + path);

        return commands;
    }

    const std::string& path() const { return path_; }

private:
    std::string   path_;
    std::ofstream out_;
};

}