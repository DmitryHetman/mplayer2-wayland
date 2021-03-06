/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/// \file
/// \ingroup Config

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "talloc.h"

#include "m_config.h"
#include "m_option.h"
#include "mp_msg.h"

#define MAX_PROFILE_DEPTH 20

static int parse_include(struct m_config *config, struct bstr param, bool set)
{
    if (param.len == 0)
        return M_OPT_MISSING_PARAM;
    if (!set)
        return 1;
    char *filename = bstrdup0(NULL, param);
    config->includefunc(config, filename);
    talloc_free(filename);
    return 1;
}

static int parse_profile(struct m_config *config, const struct m_option *opt,
                         struct bstr name, struct bstr param, bool set)
{
    if (!bstrcmp0(param, "help")) {
        struct m_profile *p;
        if (!config->profiles) {
            mp_tmsg(MSGT_CFGPARSER, MSGL_INFO,
                    "No profiles have been defined.\n");
            return M_OPT_EXIT - 1;
        }
        mp_tmsg(MSGT_CFGPARSER, MSGL_INFO, "Available profiles:\n");
        for (p = config->profiles; p; p = p->next)
            mp_msg(MSGT_CFGPARSER, MSGL_INFO, "\t%s\t%s\n", p->name,
                   p->desc ? p->desc : "");
        mp_msg(MSGT_CFGPARSER, MSGL_INFO, "\n");
        return M_OPT_EXIT - 1;
    }

    char **list = NULL;
    int r = m_option_type_string_list.parse(opt, name, param, false, &list,
                                            NULL);
    if (r < 0)
        return r;
    if (!list || !list[0])
        return M_OPT_INVALID;
    for (int i = 0; list[i]; i++) {
        struct m_profile *p = m_config_get_profile(config, list[i]);
        if (!p) {
            mp_tmsg(MSGT_CFGPARSER, MSGL_WARN, "Unknown profile '%s'.\n",
                    list[i]);
            r = M_OPT_INVALID;
        } else if (set)
            m_config_set_profile(config, p);
    }
    m_option_free(opt, &list);
    return r;
}

static int show_profile(struct m_option *opt, char *name, char *param)
{
    struct m_config *config = opt->priv;
    struct m_profile *p;
    int i, j;
    if (!param)
        return M_OPT_MISSING_PARAM;
    if (!(p = m_config_get_profile(config, param))) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_ERR, "Unknown profile '%s'.\n", param);
        return M_OPT_EXIT - 1;
    }
    if (!config->profile_depth)
        mp_tmsg(MSGT_CFGPARSER, MSGL_INFO, "Profile %s: %s\n", param,
                p->desc ? p->desc : "");
    config->profile_depth++;
    for (i = 0; i < p->num_opts; i++) {
        char spc[config->profile_depth + 1];
        for (j = 0; j < config->profile_depth; j++)
            spc[j] = ' ';
        spc[config->profile_depth] = '\0';

        mp_msg(MSGT_CFGPARSER, MSGL_INFO, "%s%s=%s\n", spc,
               p->opts[2 * i], p->opts[2 * i + 1]);

        if (config->profile_depth < MAX_PROFILE_DEPTH
            && !strcmp(p->opts[2*i], "profile")) {
            char *e, *list = p->opts[2 * i + 1];
            while ((e = strchr(list, ','))) {
                int l = e - list;
                char tmp[l+1];
                if (!l)
                    continue;
                memcpy(tmp, list, l);
                tmp[l] = '\0';
                show_profile(opt, name, tmp);
                list = e + 1;
            }
            if (list[0] != '\0')
                show_profile(opt, name, list);
        }
    }
    config->profile_depth--;
    if (!config->profile_depth)
        mp_msg(MSGT_CFGPARSER, MSGL_INFO, "\n");
    return M_OPT_EXIT - 1;
}

static int list_options(struct m_option *opt, char *name, char *param)
{
    struct m_config *config = opt->priv;
    m_config_print_option_list(config);
    return M_OPT_EXIT;
}

static void m_option_save(const struct m_config *config,
                          const struct m_option *opt, void *dst)
{
    if (opt->type->copy) {
        const void *src = m_option_get_ptr(opt, config->optstruct);
        opt->type->copy(opt, dst, src, NULL);
    }
}

static void m_option_set(void *optstruct,
                         const struct m_option *opt, const void *src)
{
    if (opt->type->copy) {
        void *dst = m_option_get_ptr(opt, optstruct);
        opt->type->copy(opt, dst, src, optstruct);
    }
}



static void m_config_add_option(struct m_config *config,
                                const struct m_option *arg,
                                const char *prefix, char *disabled_feature);

struct m_config *m_config_new(void *optstruct,
                              int includefunc(struct m_config *conf,
                                              char *filename))
{
    struct m_config *config;
    static const struct m_option ref_opts[] = {
        { "profile", NULL, CONF_TYPE_STRING_LIST, CONF_NOSAVE, 0, 0, NULL },
        { "show-profile", show_profile, CONF_TYPE_PRINT_FUNC, CONF_NOCFG },
        { "list-options", list_options, CONF_TYPE_PRINT_FUNC, CONF_NOCFG },
        { NULL }
    };

    config = talloc_zero(NULL, struct m_config);
    config->full = true;
    config->lvl = 1; // 0 Is the defaults
    struct m_option *self_opts = talloc_memdup(config, ref_opts,
                                               sizeof(ref_opts));
    for (int i = 1; self_opts[i].name; i++)
        self_opts[i].priv = config;
    m_config_register_options(config, self_opts);
    if (includefunc) {
        struct m_option *p = talloc_ptrtype(config, p);
        *p = (struct m_option){
            "include", NULL, CONF_TYPE_STRING, CONF_NOSAVE,
        };
        m_config_add_option(config, p, NULL, NULL);
        config->includefunc = includefunc;
    }
    config->optstruct = optstruct;

    return config;
}

struct m_config *m_config_simple(const struct m_option *options)
{
    struct m_config *config = talloc_zero(NULL, struct m_config);
    m_config_register_options(config, options);
    return config;
}

void m_config_free(struct m_config *config)
{
    assert(config->full);   // use talloc_free() for simple
    struct m_config_option *copt;
    for (copt = config->opts; copt; copt = copt->next) {
        if (copt->flags & M_CFG_OPT_ALIAS)
            continue;
        if (copt->opt->type->flags & M_OPT_TYPE_DYNAMIC) {
            void *ptr = m_option_get_ptr(copt->opt, config->optstruct);
            if (ptr)
                m_option_free(copt->opt, ptr);
        }
        struct m_config_save_slot *sl;
        for (sl = copt->slots; sl; sl = sl->prev)
            m_option_free(copt->opt, sl->data);
    }
    talloc_free(config);
}

void m_config_initialize(struct m_config *config, void *optstruct)
{
    struct m_config_option *copt;
    for (copt = config->opts; copt; copt = copt->next) {
        const struct m_option *opt = copt->opt;
        if (!opt->defval)
            continue;
        m_option_set(optstruct, opt, opt->defval);
    }
}

void m_config_push(struct m_config *config)
{
    struct m_config_option *co;
    struct m_config_save_slot *slot;

    assert(config != NULL);
    assert(config->lvl > 0);

    config->lvl++;

    for (co = config->opts; co; co = co->next) {
        if (co->opt->type->flags & M_OPT_TYPE_HAS_CHILD)
            continue;
        if (co->opt->flags & (M_OPT_GLOBAL | M_OPT_NOSAVE))
            continue;
        if (co->flags & M_CFG_OPT_ALIAS)
            continue;

        // Update the current status
        m_option_save(config, co->opt, co->slots->data);

        // Allocate a new slot
        slot = talloc_zero_size(co, sizeof(struct m_config_save_slot) +
                                co->opt->type->size);
        slot->lvl = config->lvl;
        slot->prev = co->slots;
        co->slots = slot;
        m_option_copy(co->opt, co->slots->data, co->slots->prev->data);
        // Reset our set flag
        co->flags &= ~M_CFG_OPT_SET;
    }

    mp_msg(MSGT_CFGPARSER, MSGL_DBG2,
           "Config pushed level is now %d\n", config->lvl);
}

void m_config_pop(struct m_config *config)
{
    struct m_config_option *co;
    struct m_config_save_slot *slot;

    assert(config != NULL);
    assert(config->lvl > 1);

    for (co = config->opts; co; co = co->next) {
        int pop = 0;
        if (co->opt->type->flags & M_OPT_TYPE_HAS_CHILD)
            continue;
        if (co->opt->flags & (M_OPT_GLOBAL | M_OPT_NOSAVE))
            continue;
        if (co->flags & M_CFG_OPT_ALIAS)
            continue;
        if (co->slots->lvl > config->lvl)
            mp_msg(MSGT_CFGPARSER, MSGL_WARN,
                   "Save slot found from lvl %d is too old: %d !!!\n",
                   config->lvl, co->slots->lvl);

        while (co->slots->lvl >= config->lvl) {
            m_option_free(co->opt, co->slots->data);
            slot = co->slots;
            co->slots = slot->prev;
            talloc_free(slot);
            pop++;
        }
        if (pop) // We removed some ctx -> set the previous value
            m_option_set(config->optstruct, co->opt, co->slots->data);
    }

    config->lvl--;
    mp_msg(MSGT_CFGPARSER, MSGL_DBG2, "Config poped level=%d\n", config->lvl);
}

static void add_options(struct m_config *config, const struct m_option *defs,
                        const char *prefix, char *disabled_feature)
{
    char *dis = disabled_feature;
    const char marker[] = "conditional functionality: ";
    for (int i = 0; defs[i].name; i++) {
        if (!strncmp(defs[i].name, marker, strlen(marker))) {
            // If a subconfig entry itself is disabled, everything
            // under it is already disabled for the same reason.
            if (!disabled_feature) {
                if (!strcmp(defs[i].name + strlen(marker), "1"))
                    dis = NULL;
                else
                    dis = defs[i].p;
            }
            continue;
        }
        m_config_add_option(config, defs + i, prefix, dis);
    }
}

static void m_config_add_option(struct m_config *config,
                                const struct m_option *arg, const char *prefix,
                                char *disabled_feature)
{
    struct m_config_option *co;
    struct m_config_save_slot *sl;

    assert(config != NULL);
    assert(config->lvl > 0 || !config->full);
    assert(arg != NULL);

    // Allocate a new entry for this option
    co = talloc_zero_size(config,
                          sizeof(struct m_config_option) + arg->type->size);
    co->opt = arg;
    co->disabled_feature = disabled_feature;

    // Fill in the full name
    if (prefix && *prefix)
        co->name = talloc_asprintf(co, "%s:%s", prefix, arg->name);
    else
        co->name = (char *)arg->name;

    // Option with children -> add them
    if (arg->type->flags & M_OPT_TYPE_HAS_CHILD) {
        add_options(config, arg->p, co->name, disabled_feature);
    } else {
        struct m_config_option *i;
        // Check if there is already an option pointing to this address
        if (arg->p || arg->new && arg->offset >= 0) {
            for (i = config->opts; i; i = i->next) {
                if (arg->new ? (i->opt->new && i->opt->offset == arg->offset)
                    : (!i->opt->new && i->opt->p == arg->p)) {
                    // So we don't save the same vars more than 1 time
                    co->slots = i->slots;
                    co->flags |= M_CFG_OPT_ALIAS;
                    break;
                }
            }
        }
        if (config->full && !(co->flags & M_CFG_OPT_ALIAS)) {
            // Allocate a slot for the defaults
            sl = talloc_zero_size(co, sizeof(struct m_config_save_slot) +
                                  arg->type->size);
            m_option_save(config, arg, sl->data);
            // Hack to avoid too much trouble with dynamically allocated data:
            // We replace original default and always use a dynamic version
            if (!arg->new && (arg->type->flags & M_OPT_TYPE_DYNAMIC)) {
                char **hackptr = m_option_get_ptr(arg, config->optstruct);
                if (hackptr && *hackptr) {
                    *hackptr = NULL;
                    m_option_set(config->optstruct, arg, sl->data);
                }
            }
            sl->lvl = 0;
            sl->prev = NULL;
            co->slots = talloc_zero_size(co, sizeof(struct m_config_save_slot) +
                                         arg->type->size);
            co->slots->prev = sl;
            co->slots->lvl = config->lvl;
            m_option_copy(co->opt, co->slots->data, sl->data);
        }
    }
    co->next = config->opts;
    config->opts = co;
}

int m_config_register_options(struct m_config *config,
                              const struct m_option *args)
{
    assert(config != NULL);
    assert(config->lvl > 0 || !config->full);
    assert(args != NULL);

    add_options(config, args, NULL, NULL);

    return 1;
}

static struct m_config_option *m_config_get_co(const struct m_config *config,
                                               struct bstr name)
{
    struct m_config_option *co;

    for (co = config->opts; co; co = co->next) {
        struct bstr coname = bstr(co->name);
        if ((co->opt->type->flags & M_OPT_TYPE_ALLOW_WILDCARD)
                && bstr_endswith0(coname, "*")) {
            coname.len--;
            if (bstrcasecmp(bstr_splice(name, 0, coname.len), coname) == 0)
                return co;
        } else if (bstrcasecmp(coname, name) == 0)
            return co;
    }
    return NULL;
}

static int parse_subopts(struct m_config *config, void *optstruct, char *name,
                         char *prefix, struct bstr param, bool set);

static int m_config_parse_option(struct m_config *config, void *optstruct,
                                 struct bstr name, struct bstr param,
                                 bool ambiguous_param, bool set)
{
    assert(config != NULL);
    assert(config->lvl > 0 || !config->full);
    assert(name.len != 0);

    struct m_config_option *co = m_config_get_co(config, name);
    if (!co)
        return M_OPT_UNKNOWN;
    if (co->disabled_feature) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                "Option \"%.*s\" is not available in this version of mplayer2, "
                "because it has been compiled with feature \"%s\" disabled.\n",
                BSTR_P(name), co->disabled_feature);
        return M_OPT_UNKNOWN;
    }

    // This is the only mandatory function
    assert(co->opt->type->parse);

    // Check if this option isn't forbidden in the current mode
    if ((config->mode == M_CONFIG_FILE) && (co->opt->flags & M_OPT_NOCFG)) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                "The %.*s option can't be used in a config file.\n",
                BSTR_P(name));
        return M_OPT_INVALID;
    }
    if ((config->mode == M_COMMAND_LINE) && (co->opt->flags & M_OPT_NOCMD)) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                "The %.*s option can't be used on the command line.\n",
                BSTR_P(name));
        return M_OPT_INVALID;
    }
    // During command line preparse set only pre-parse options
    // Otherwise only set pre-parse option if they were not already set.
    if (((config->mode == M_COMMAND_LINE_PRE_PARSE) &&
         !(co->opt->flags & M_OPT_PRE_PARSE)) ||
        ((config->mode != M_COMMAND_LINE_PRE_PARSE) &&
         (co->opt->flags & M_OPT_PRE_PARSE) && (co->flags & M_CFG_OPT_SET)))
        set = 0;

    if (config->includefunc && !bstrcmp0(name, "include")) {
        return parse_include(config, param, set);
    } else if (!bstrcmp0(name, "profile"))
        return parse_profile(config, co->opt, name, param, set);

    // Option with children are a bit different to parse
    if (co->opt->type->flags & M_OPT_TYPE_HAS_CHILD) {
        char prefix[110];
        assert(strlen(co->name) < 100);
        sprintf(prefix, "%s:", co->name);
        return parse_subopts(config, optstruct, co->name, prefix, param, set);
    }

    void *dst = set ? m_option_get_ptr(co->opt, optstruct) : NULL;
    int r = co->opt->type->parse(co->opt, name, param, ambiguous_param, dst,
                                 optstruct);
    // Parsing failed ?
    if (r < 0)
        return r;
    else if (set)
        co->flags |= M_CFG_OPT_SET;

    return r;
}

static int parse_subopts(struct m_config *config, void *optstruct, char *name,
                         char *prefix, struct bstr param, bool set)
{
    char **lst = NULL;
    // Split the argument into child options
    int r = m_option_type_subconfig.parse(NULL, bstr(""), param, false, &lst,
                                          optstruct);
    if (r < 0)
        return r;
    // Parse the child options
    for (int i = 0; lst && lst[2 * i]; i++) {
        // Build the full name
        char n[110];
        if (snprintf(n, 110, "%s%s", prefix, lst[2 * i]) > 100)
            abort();
        if (!m_config_get_option(config, bstr(n))) {
            if (strncmp(lst[2 * i], "no-", 3))
                goto nosubopt;
            snprintf(n, 110, "%s%s", prefix, lst[2 * i] + 3);
            const struct m_option *o = m_config_get_option(config, bstr(n));
            if (!o || o->type != &m_option_type_flag) {
            nosubopt:
                mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                        "Error: option '%s' has no suboption '%s'.\n",
                        name, lst[2 * i]);
                r = M_OPT_INVALID;
                break;
            }
            if (lst[2 * i + 1]) {
                mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                        "A --no-* option can't take parameters: "
                        "%s=%s\n", lst[2 * i], lst[2 * i + 1]);
                r = M_OPT_INVALID;
                break;
            }
            lst[2 * i + 1] = "no";
        }
        int sr = m_config_parse_option(config, optstruct, bstr(n),
                                       bstr(lst[2 * i + 1]), false, set);
        if (sr < 0) {
            if (sr == M_OPT_MISSING_PARAM) {
                mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                        "Error: suboption '%s' of '%s' must have "
                        "a parameter!\n", lst[2 * i], name);
                r = M_OPT_INVALID;
            } else
                r = sr;
            break;
        }
    }
    talloc_free(lst);
    return r;
}

int m_config_set_option(struct m_config *config, struct bstr name,
                                 struct bstr param, bool ambiguous_param)
{
    mp_msg(MSGT_CFGPARSER, MSGL_DBG2, "Setting %.*s=%.*s\n", BSTR_P(name),
           BSTR_P(param));
    return m_config_parse_option(config, config->optstruct, name, param,
                                 ambiguous_param, true);
}

int m_config_check_option(struct m_config *config, struct bstr name,
                          struct bstr param, bool ambiguous_param)
{
    int r;
    mp_msg(MSGT_CFGPARSER, MSGL_DBG2, "Checking %.*s=%.*s\n", BSTR_P(name),
           BSTR_P(param));
    r = m_config_parse_option(config, NULL, name, param, ambiguous_param, 0);
    if (r == M_OPT_MISSING_PARAM) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_ERR,
                "Error: option '%.*s' must have a parameter!\n", BSTR_P(name));
        return M_OPT_INVALID;
    }
    return r;
}

int m_config_parse_suboptions(struct m_config *config, void *optstruct,
                              char *name, char *subopts)
{
    if (!subopts || !*subopts)
        return 0;
    return parse_subopts(config, optstruct, name, "", bstr(subopts), true);
}


const struct m_option *m_config_get_option(const struct m_config *config,
                                           struct bstr name)
{
    struct m_config_option *co;

    assert(config != NULL);
    assert(config->lvl > 0 || !config->full);

    co = m_config_get_co(config, name);
    if (co)
        return co->opt;
    else
        return NULL;
}

void m_config_print_option_list(const struct m_config *config)
{
    char min[50], max[50];
    struct m_config_option *co;
    int count = 0;

    if (!config->opts)
        return;

    mp_tmsg(MSGT_CFGPARSER, MSGL_INFO,
            "\n Name                 Type            Min        Max      Global  CL    Cfg\n\n");
    for (co = config->opts; co; co = co->next) {
        const struct m_option *opt = co->opt;
        if (opt->type->flags & M_OPT_TYPE_HAS_CHILD)
            continue;
        if (opt->flags & M_OPT_MIN)
            sprintf(min, "%-8.0f", opt->min);
        else
            strcpy(min, "No");
        if (opt->flags & M_OPT_MAX)
            sprintf(max, "%-8.0f", opt->max);
        else
            strcpy(max, "No");
        mp_msg(MSGT_CFGPARSER, MSGL_INFO,
             " %-20.20s %-15.15s %-10.10s %-10.10s %-3.3s   %-3.3s   %-3.3s\n",
               co->name,
               co->opt->type->name,
               min,
               max,
               opt->flags & CONF_GLOBAL ? "Yes" : "No",
               opt->flags & CONF_NOCMD ? "No" : "Yes",
               opt->flags & CONF_NOCFG ? "No" : "Yes");
        count++;
    }
    mp_tmsg(MSGT_CFGPARSER, MSGL_INFO, "\nTotal: %d options\n", count);
}

struct m_profile *m_config_get_profile(const struct m_config *config,
                                       char *name)
{
    struct m_profile *p;
    for (p = config->profiles; p; p = p->next)
        if (!strcmp(p->name, name))
            return p;
    return NULL;
}

struct m_profile *m_config_add_profile(struct m_config *config, char *name)
{
    struct m_profile *p = m_config_get_profile(config, name);
    if (p)
        return p;
    p = talloc_zero(config, struct m_profile);
    p->name = talloc_strdup(p, name);
    p->next = config->profiles;
    config->profiles = p;
    return p;
}

void m_profile_set_desc(struct m_profile *p, char *desc)
{
    talloc_free(p->desc);
    p->desc = talloc_strdup(p, desc);
}

int m_config_set_profile_option(struct m_config *config, struct m_profile *p,
                                char *name, char *val)
{
    int i = m_config_check_option0(config, name, val, false);
    if (i < 0)
        return i;
    p->opts = talloc_realloc(p, p->opts, char *, 2 * (p->num_opts + 2));
    p->opts[p->num_opts * 2] = talloc_strdup(p, name);
    p->opts[p->num_opts * 2 + 1] = talloc_strdup(p, val);
    p->num_opts++;
    p->opts[p->num_opts * 2] = p->opts[p->num_opts * 2 + 1] = NULL;
    return 1;
}

void m_config_set_profile(struct m_config *config, struct m_profile *p)
{
    int i;
    if (config->profile_depth > MAX_PROFILE_DEPTH) {
        mp_tmsg(MSGT_CFGPARSER, MSGL_WARN,
                "WARNING: Profile inclusion too deep.\n");
        return;
    }
    int prev_mode = config->mode;
    config->mode = M_CONFIG_FILE;
    config->profile_depth++;
    for (i = 0; i < p->num_opts; i++)
        m_config_set_option0(config, p->opts[2 * i], p->opts[2 * i + 1], false);
    config->profile_depth--;
    config->mode = prev_mode;
}
