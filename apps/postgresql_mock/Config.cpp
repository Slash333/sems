#include "Config.h"
#include "PostgreSqlMock.h"

#include <fstream>
#include <string>

using std::string;

int map_func(cfg_t *cfg, cfg_opt_t *, int argc, const char **argv)
{
    if(argc < 2)
        return 1;

    const char* query = argv[0];
    const char* response = argv[1];
    const char* error = argc > 2 ? argv[2] : "";
    const bool timeout = argc > 3 ? (strcmp(argv[3], "true") == 0) : false;
    PostgreSqlMock::instance()->insert_resp_map(query, response, error, timeout);
    return 0;
}

int map_file_func(cfg_t *cfg, cfg_opt_t *, int argc, const char **argv)
{
    if(argc != 2)
        return 1;

    const char* query = argv[0];
    const char* file_path = argv[1];

    string data;
    try {
        std::ifstream f(file_path);
        if(!f) {
            DBG("failed to open: %s", file_path);
            return 1;
        }

        data = string((std::istreambuf_iterator<char>(f)),
                      (std::istreambuf_iterator<char>()));
    } catch(...) {
        DBG("failed to load %s", file_path);
        return 1;
    }

    PostgreSqlMock::instance()->insert_resp_map(query, data);
    return 0;
}

cfg_opt_t pg_opts[] =
{
    CFG_FUNC(CFG_OPT_MAP, &map_func),
    CFG_FUNC(CFG_OPT_MAP_FILE, &map_file_func),
    CFG_END()
};
