#ifndef COMMAND_H
#define COMMAND_H

#include <string>
#include <vector>
#include <functional>
#include "db.h"

class CommandTable {
public:
    CommandTable(Database* db);
    std::string execute(const std::vector<std::string>& args);

private:
    Database* db;
    std::unordered_map<std::string, std::function<std::string(const std::vector<std::string>&)>> handlers;

    // RESP 响应构造辅助
    static std::string ok_resp();
    static std::string err_resp(const std::string& msg);
    static std::string int_resp(int val);
    static std::string bulk_resp(const std::string& s);
    static std::string null_bulk_resp();
    static std::string array_resp(const std::vector<std::string>& items);
    static std::string ping_resp(const std::string& msg);

    void register_commands();
};

#endif // COMMAND_H
