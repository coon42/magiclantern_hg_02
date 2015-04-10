
#include <dryos.h>
#include <string.h>

#include "lua_common.h"

extern int last_keypress;
int module_send_keypress(int module_key);

static int luaCB_key_press(lua_State * L)
{
    LUA_PARAM_INT(key, 1);
    module_send_keypress(key);
    return 0;
}

static int luaCB_key_wait(lua_State * L)
{
    LUA_PARAM_INT_OPTIONAL(key, 1, 0);
    LUA_PARAM_INT_OPTIONAL(timeout, 1, 0);
    timeout *= 10;
    last_keypress = 0;
    int time = 0;
    //TODO: probably better to use a semaphore
    while((key && last_keypress != key) || (!key && !last_keypress))
    {
        msleep(100);
        if(timeout && time++ > timeout)
        {
            lua_pushinteger(L, 0);
            return 1;
        }
    }
    lua_pushinteger(L, last_keypress);
    return 1;
}

static int luaCB_key_index(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "last")) lua_pushinteger(L, last_keypress);
    else lua_rawget(L, 1);
    return 1;
}

static int luaCB_key_newindex(lua_State * L)
{
    LUA_PARAM_STRING_OPTIONAL(key, 2, "");
    if(!strcmp(key, "last"))
    {
        return luaL_error(L, "'%s' is readonly!", key);
    }
    else
    {
        lua_rawset(L, 1);
    }
    return 0;
}

const luaL_Reg keylib[] =
{
    {"press", luaCB_key_press},
    {"wait", luaCB_key_wait},
    {NULL, NULL}
};

LUA_LIB(key)
