/*
 * draw.c — kicli sch place / net / nc / sheet
 *
 * Label-first schematic authoring:
 *
 *   place  adds a (symbol …) from the bundled KiCad library catalog.
 *   net    attaches a label or power port at each specified pin's world
 *          position. Wires are never emitted; every connection is a label.
 *   nc     places (no_connect …) markers at pin positions.
 *   sheet  adds a hierarchical sub-sheet (creates the child .kicad_sch).
 *
 * Design philosophy: the agent provides a logical spec (components + nets);
 * kicli emits valid KiCad 10 s-expression with correct pin positions,
 * grid-snapped placements, and proper UUIDs. No 2-D layout optimization.
 * Output is electrically correct but cosmetically utilitarian — the
 * human (or GUI) rearranges if they want prettiness.
 *
 * Shared sexpr builders / walkers live in sch_common.c (sch_ops.h).
 */

#include "kicli/sch_ops.h"
#include "kicli/error.h"
#include "kicli/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

/* ── cmd_sch_place ──────────────────────────────────────────────────────── */

int cmd_sch_place(const char *sch_path, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
          "Usage: kicli sch <file> place <lib_id> <ref|?> [<value>]\n"
          "                   [--at X,Y] [--angle {0|90|180|270}] [--mirror x|y]\n"
          "                   [--footprint FP]\n"
          "\n"
          "Places a symbol from the bundled KiCad library catalog. If --at is\n"
          "omitted, kicli auto-picks a grid slot (10 columns × N rows).\n"
          "Pass `?` as ref to auto-annotate the next free <PREFIX><N>.\n"
          "All coordinates snap to the 1.27 mm grid so library pins\n"
          "electrically align with future labels.\n");
        return 1;
    }

    const char *lib_id = argv[0];
    const char *ref_in = argv[1];
    const char *value  = (argc >= 3 && argv[2][0] != '-') ? argv[2] : ref_in;
    char ref_buf[32];
    const char *ref = ref_in;

    double at_x = 0, at_y = 0, at_a = 0;
    int have_at = 0, mirror_x = 0, mirror_y = 0;
    const char *footprint = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf", &at_x, &at_y) != 2) {
                fprintf(stderr, "error: --at expects X,Y (e.g. --at 100,50)\n");
                return 1;
            }
            have_at = 1;
        } else if (strcmp(argv[i], "--angle") == 0 && i + 1 < argc) {
            at_a = atof(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (*m == 'x' || *m == 'X') mirror_x = 1;
            else if (*m == 'y' || *m == 'Y') mirror_y = 1;
        } else if (strcmp(argv[i], "--footprint") == 0 && i + 1 < argc) {
            footprint = argv[++i];
        }
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    /* `?` ref → auto-annotate. Prefix derived from lib_id (Device:R → "R"). */
    if (strcmp(ref_in, "?") == 0) {
        char prefix[8] = "U";
        const char *colon = strchr(lib_id, ':');
        const char *base = colon ? colon + 1 : lib_id;
        if (base[0]) {
            size_t pl = 0;
            for (; pl < 3 && isalpha((unsigned char)base[pl]); pl++)
                prefix[pl] = (char)toupper((unsigned char)base[pl]);
            prefix[pl ? pl : 1] = '\0';
        }
        int max = 0;
        for (size_t i = 0; i < root->num_children; i++) {
            sexpr_t *c = root->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
            if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
            const char *ex = placed_property(c, "Reference");
            if (!ex) continue;
            size_t pl = strlen(prefix);
            if (strncmp(ex, prefix, pl) != 0) continue;
            int n = atoi(ex + pl);
            if (n > max) max = n;
        }
        snprintf(ref_buf, sizeof(ref_buf), "%s%d", prefix, max + 1);
        ref = ref_buf;
        if (value == ref_in) value = ref_buf;
    }

    if (find_placed_by_ref(root, ref)) {
        fprintf(stderr, "error: reference '%s' already exists — pick a different one\n", ref);
        sexpr_free(root); return 1;
    }

    int rc = ensure_lib_symbol_in_sheet(root, lib_id);
    if (rc != 0) {
        fprintf(stderr, "error: library symbol '%s' not found in bundled catalog\n", lib_id);
        fprintf(stderr, "  (project-local libs not yet supported by `place` — see roadmap)\n");
        sexpr_free(root); return 2;
    }
    sexpr_t *lib_def = find_lib_symbol_node(root, lib_id);

    if (!have_at) {
        size_t idx = count_placed_symbols(root);
        auto_slot(idx, &at_x, &at_y);
    }
    at_x = snap_grid(at_x, 1.27);
    at_y = snap_grid(at_y, 1.27);

    const char *root_uuid = get_root_uuid(root);
    char proj[128];
    project_name_for(sch_path, proj, sizeof(proj));

    sexpr_t *sym = mk_placed_symbol(lib_id, ref, value, at_x, at_y, at_a,
                                     lib_def, root, root_uuid, proj);

    if (mirror_x || mirror_y) {
        sexpr_t *m = sexpr_make_list();
        sexpr_list_append(m, make_atom("mirror"));
        sexpr_list_append(m, make_atom(mirror_x ? "x" : "y"));
        sexpr_list_append(sym, m);
    }

    /* --footprint overrides the inherited library default. */
    if (footprint && *footprint) {
        for (size_t i = 1; i < sym->num_children; i++) {
            sexpr_t *p = sym->children[i];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
            if (p->children[1]->value && strcmp(p->children[1]->value, "Footprint") == 0) {
                free(p->children[2]->value);
                p->children[2]->value = strdup(footprint);
                break;
            }
        }
    }

    sexpr_list_append(root, sym);

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("placed %s at %.2f,%.2f (%s)\n", ref, at_x, at_y, lib_id);
    sexpr_free(root);
    return 0;
}

/* ── cmd_sch_net ────────────────────────────────────────────────────────── */

int cmd_sch_net(const char *sch_path, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
          "Usage: kicli sch <file> net <net-name> <ref>:<pin> [<ref>:<pin> ...]\n"
          "                   [--as local|global|hier|power]\n"
          "\n"
          "Attaches a label (or power port for known rails, or hierarchical\n"
          "label with --as hier) at each pin's world position. Label rotation\n"
          "matches the pin's outward angle so text reads away from the symbol.\n"
          "\n"
          "Power rail heuristic (override with --as):\n"
          "  GND, VCC, VDD, VSS, VEE, GNDA, AGND, PGND, +BATT, -BATT, EARTH\n"
          "  +3V3, +5V, +12V, -12V, 3V3, 5V, 12V, +1V8, +3.3V …\n");
        return 1;
    }

    const char *net  = argv[0];
    const char *force_as = NULL;
    int pin_arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--as") == 0 && i + 1 < argc)
            force_as = argv[++i];
    }

    char canon[96];
    int is_power = detect_power(net, canon, sizeof(canon));
    if (force_as) {
        if (strcmp(force_as, "power") == 0) {
            is_power = 1;
            if (!canon[0]) snprintf(canon, sizeof(canon), "%s", net);
        } else {
            is_power = 0;
        }
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    seed_pwr_counter(root);

    const char *root_uuid = get_root_uuid(root);
    char proj[128];
    project_name_for(sch_path, proj, sizeof(proj));

    if (is_power) {
        char lib_id[128];
        snprintf(lib_id, sizeof(lib_id), "power:%s", canon);
        if (ensure_lib_symbol_in_sheet(root, lib_id) != 0) {
            fprintf(stderr,
                "warning: power rail '%s' not found in bundled `power` library;\n"
                "         falling back to a (label \"%s\") instead\n",
                canon, net);
            is_power = 0;
        }
    }

    int n_pins = 0, n_err = 0;
    for (int i = pin_arg_start; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--as") == 0 && i + 1 < argc) i++;
            continue;
        }
        char refpin[128];
        snprintf(refpin, sizeof(refpin), "%s", argv[i]);
        char *colon = strchr(refpin, ':');
        if (!colon) {
            fprintf(stderr, "error: expected REF:PIN, got '%s'\n", argv[i]);
            n_err++; continue;
        }
        *colon = '\0';
        const char *ref = refpin;
        const char *pin = colon + 1;

        double wx, wy, wa;
        int from_sheet = 0;
        if (world_pin_pos(root, ref, pin, &wx, &wy, &wa) != 0) {
            /* Try sheet-pin lookup: SheetName:PinName for hierarchical sheets. */
            if (sheet_pin_pos(root, ref, pin, &wx, &wy, &wa) != 0) {
                fprintf(stderr, "error: %s\n", kicli_last_error());
                n_err++; continue;
            }
            from_sheet = 1;
        }

        wx = snap_grid(wx, 1.27);
        wy = snap_grid(wy, 1.27);

        /* Label rotation — for clean readability, text reads OUTWARD from
         * the symbol. world_pin_pos returns wa = INTO direction, so the
         * outward label angle is wa+180. KiCad label angle 0 = text reads
         * left-to-right with anchor on the left of the text — perfect for
         * a pin extending RIGHT (outward=0). For an upward pin (outward=90)
         * the label rotates 90° to extend up. */
        double label_angle = fmod(wa + 180.0 + 720.0, 360.0);

        sexpr_t *primitive;
        if (from_sheet) {
            /* Sheet pin already shows its own name text. Place the
             * splicing label 2.54 mm OUTWARD with a wire stub, same as
             * the power-port pattern, so we don't draw "VIN VIN" stacked
             * on top of itself. */
            double dx, dy;
            offset_outward(wa, 2.54, &dx, &dy);
            double lx = snap_grid(wx + dx, 1.27);
            double ly = snap_grid(wy + dy, 1.27);
            sexpr_list_append(root, mk_wire(wx, wy, lx, ly));
            primitive = mk_label(net, lx, ly, label_angle);
        } else if (is_power) {
            /* CCM7 pattern: place power port at a 2.54 mm offset in the
             * outward direction + a wire stub between. Avoids overlap
             * with the connecting symbol's body / pin numbers. */
            double pp_x, pp_y;
            primitive = mk_power_port(root, canon, wx, wy, wa,
                                      root_uuid, proj, &pp_x, &pp_y);
            sexpr_list_append(root, mk_wire(wx, wy, pp_x, pp_y));
        } else if (force_as && (strcmp(force_as, "hier") == 0 ||
                                strcmp(force_as, "hierarchical") == 0)) {
            primitive = mk_hier_label(net, "passive", wx, wy, label_angle);
        } else if (force_as && strcmp(force_as, "global") == 0) {
            primitive = mk_global_label(net, "passive", wx, wy, label_angle);
        } else {
            primitive = mk_label(net, wx, wy, label_angle);
        }
        sexpr_list_append(root, primitive);
        n_pins++;
    }

    if (n_pins == 0) {
        fprintf(stderr, "error: no valid pins specified\n");
        sexpr_free(root);
        return 1;
    }

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("net '%s' attached to %d pin(s)%s%s\n",
           net, n_pins,
           is_power ? " (power port)" : " (label)",
           n_err ? " — some pins were skipped, see warnings above" : "");
    sexpr_free(root);
    return n_err ? 1 : 0;
}

/* ── cmd_sch_nc ─────────────────────────────────────────────────────────── */

int cmd_sch_nc(const char *sch_path, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr,
          "Usage: kicli sch <file> nc <ref>:<pin> [<ref>:<pin> ...]\n"
          "\n"
          "Places (no_connect) markers at the given pin positions. Use this\n"
          "on pins you deliberately leave floating — it silences ERC.\n");
        return 1;
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    int n_pins = 0, n_err = 0;
    for (int i = 0; i < argc; i++) {
        char refpin[128];
        snprintf(refpin, sizeof(refpin), "%s", argv[i]);
        char *colon = strchr(refpin, ':');
        if (!colon) {
            fprintf(stderr, "error: expected REF:PIN, got '%s'\n", argv[i]);
            n_err++; continue;
        }
        *colon = '\0';
        const char *ref = refpin;
        const char *pin = colon + 1;

        double wx, wy, wa;
        if (world_pin_pos(root, ref, pin, &wx, &wy, &wa) != 0) {
            fprintf(stderr, "error: %s\n", kicli_last_error());
            n_err++; continue;
        }
        wx = snap_grid(wx, 1.27);
        wy = snap_grid(wy, 1.27);
        sexpr_list_append(root, mk_no_connect(wx, wy));
        n_pins++;
    }

    if (n_pins == 0) {
        fprintf(stderr, "error: no valid pins specified\n");
        sexpr_free(root);
        return 1;
    }

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("NC marker attached to %d pin(s)\n", n_pins);
    sexpr_free(root);
    return n_err ? 1 : 0;
}

/* ── cmd_sch_sheet ─────────────────────────────────────────────────────────
 *
 * Adds a hierarchical sheet to <parent>:
 *   1. Creates <child-file> with a blank kicad_sch (same template as `kicli
 *      new`) if it doesn't already exist. Reuses an existing file as-is.
 *   2. Appends a (sheet …) block to the parent at --at (auto-grids if absent),
 *      with --size or default 30×20 mm, and a (instances (project …)) entry
 *      so KiCad can resolve the per-sheet annotation path.
 *   3. Optionally seeds sheet pins via --pins NAME[:type][,NAME[:type]…].
 *      Pins are auto-distributed along the sheet's left edge on the 2.54 mm
 *      grid. The agent should also call `kicli sch <child> net <NAME> …
 *      --as hier` so the inside of the child has matching hierarchical
 *      labels — KiCad splices them by name.
 *
 * Sheet-specific helpers (write_blank_child, mk_sheet, mk_sheet_pin,
 * count_sheets) live here because no other command needs them. If a future
 * `sheet remove`/`sheet pin add` lands they should move to sch_common.c.
 */

static int write_blank_child(const char *path, const char *title)
{
    if (kicli_exists(path)) return 0;  /* keep user-edited child intact */

    char uuid[40];
    kicli_uuid4(uuid, sizeof(uuid));

    /* Tiny static template — emit directly, no need to round-trip a sexpr. */
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
        "(kicad_sch\n"
        "\t(version 20241010)\n"
        "\t(generator \"kicli\")\n"
        "\t(generator_version \"0.1\")\n"
        "\t(uuid \"%s\")\n"
        "\t(paper \"A4\")\n"
        "\t(title_block\n"
        "\t\t(title \"%s\")\n"
        "\t\t(date \"\")\n"
        "\t\t(rev \"1\")\n"
        "\t\t(company \"\")\n"
        "\t)\n"
        "\t(lib_symbols)\n"
        "\t(sheet_instances\n"
        "\t\t(path \"/\" (page \"1\"))\n"
        "\t)\n"
        ")\n", uuid, title);
    fclose(f);
    return 0;
}

static sexpr_t *mk_sheet_pin(const char *name, const char *type,
                              double x, double y, double angle)
{
    sexpr_t *pin = sexpr_make_list();
    sexpr_list_append(pin, make_atom("pin"));
    sexpr_list_append(pin, make_str(name));
    sexpr_list_append(pin, make_atom(type ? type : "passive"));
    sexpr_list_append(pin, mk_at(x, y, angle));
    sexpr_list_append(pin, mk_effects_default());
    sexpr_list_append(pin, mk_uuid_node());
    return pin;
}

/* Build the (sheet …) block. pins[] is an optional NULL-terminated array of
 * "NAME[:TYPE]" strings; TYPE defaults to passive when omitted. */
static sexpr_t *mk_sheet(const char *name, const char *file_basename,
                         double x, double y, double w, double h,
                         const char *root_uuid, const char *project_name,
                         const char *const *pins)
{
    sexpr_t *sh = sexpr_make_list();
    sexpr_list_append(sh, make_atom("sheet"));

    char sx[32], sy[32], sw[32], shh[32];
    fmt_num(sx, sizeof(sx), x);
    fmt_num(sy, sizeof(sy), y);
    fmt_num(sw, sizeof(sw), w);
    fmt_num(shh, sizeof(shh), h);

    sexpr_t *at = sexpr_make_list();
    sexpr_list_append(at, make_atom("at"));
    sexpr_list_append(at, make_atom(sx));
    sexpr_list_append(at, make_atom(sy));
    sexpr_list_append(sh, at);

    sexpr_t *sz = sexpr_make_list();
    sexpr_list_append(sz, make_atom("size"));
    sexpr_list_append(sz, make_atom(sw));
    sexpr_list_append(sz, make_atom(shh));
    sexpr_list_append(sh, sz);

    sexpr_list_append(sh, mk_named_atom("exclude_from_sim", "no"));
    sexpr_list_append(sh, mk_named_atom("in_bom",   "yes"));
    sexpr_list_append(sh, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sh, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sh, mk_named_atom("fields_autoplaced", "yes"));

    /* (stroke (width 0.1524) (type solid)) */
    sexpr_t *stroke = sexpr_make_list();
    sexpr_list_append(stroke, make_atom("stroke"));
    sexpr_t *sw1 = sexpr_make_list();
    sexpr_list_append(sw1, make_atom("width"));
    sexpr_list_append(sw1, make_atom("0.1524"));
    sexpr_list_append(stroke, sw1);
    sexpr_t *st = sexpr_make_list();
    sexpr_list_append(st, make_atom("type"));
    sexpr_list_append(st, make_atom("solid"));
    sexpr_list_append(stroke, st);
    sexpr_list_append(sh, stroke);

    /* (fill (color 0 0 0 0.0000)) */
    sexpr_t *fill = sexpr_make_list();
    sexpr_list_append(fill, make_atom("fill"));
    sexpr_t *color = sexpr_make_list();
    sexpr_list_append(color, make_atom("color"));
    sexpr_list_append(color, make_atom("0"));
    sexpr_list_append(color, make_atom("0"));
    sexpr_list_append(color, make_atom("0"));
    sexpr_list_append(color, make_atom("0.0000"));
    sexpr_list_append(fill, color);
    sexpr_list_append(sh, fill);

    sexpr_list_append(sh, mk_uuid_node());

    sexpr_list_append(sh, mk_property("Sheetname", name,          x, y - 0.7,     0, 0));
    sexpr_list_append(sh, mk_property("Sheetfile", file_basename, x, y + h + 0.7, 0, 0));

    if (pins) {
        double py = y + 2.54;
        for (size_t i = 0; pins[i]; i++) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", pins[i]);
            char *colon = strchr(buf, ':');
            const char *pname = buf;
            const char *ptype = "passive";
            if (colon) { *colon = '\0'; ptype = colon + 1; }
            sexpr_list_append(sh, mk_sheet_pin(pname, ptype, x, py, 180.0));
            py += 2.54;
            if (py >= y + h - 1.27) py = y + 2.54;  /* wrap if more pins than fit */
        }
    }

    /* (instances (project …)) — required for KiCad to resolve per-sheet paths */
    sexpr_t *insts = sexpr_make_list();
    sexpr_list_append(insts, make_atom("instances"));
    sexpr_t *prj = sexpr_make_list();
    sexpr_list_append(prj, make_atom("project"));
    sexpr_list_append(prj, make_str(project_name ? project_name : ""));
    sexpr_t *path = sexpr_make_list();
    sexpr_list_append(path, make_atom("path"));
    char rootp[80];
    snprintf(rootp, sizeof(rootp), "/%s", root_uuid ? root_uuid : "");
    sexpr_list_append(path, make_str(rootp));
    sexpr_t *page = sexpr_make_list();
    sexpr_list_append(page, make_atom("page"));
    sexpr_list_append(page, make_str("2"));
    sexpr_list_append(path, page);
    sexpr_list_append(prj, path);
    sexpr_list_append(insts, prj);
    sexpr_list_append(sh, insts);

    return sh;
}

static size_t count_sheets(const sexpr_t *root)
{
    size_t n = 0;
    for (size_t i = 0; i < root->num_children; i++) {
        const sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        if (c->children[0]->value && strcmp(c->children[0]->value, "sheet") == 0) n++;
    }
    return n;
}

int cmd_sch_sheet(const char *sch_path, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
          "Usage: kicli sch <parent.kicad_sch> sheet <sheet-name> <child-file>\n"
          "                  [--at X,Y] [--size W,H] [--pins NAME[:type],…]\n"
          "\n"
          "Adds a hierarchical sheet to the parent. Creates <child-file>\n"
          "with a blank schematic if it doesn't exist (existing files are\n"
          "left alone). Sheet pins are auto-distributed on the left edge.\n"
          "\n"
          "Pin types: passive | input | output | bidirectional | tri_state\n"
          "         | power_in | power_out\n"
          "\n"
          "After adding pins on the parent, give the child matching\n"
          "hierarchical labels:  kicli sch <child> net NAME R1:1 --as hier\n");
        return 1;
    }

    const char *sheet_name  = argv[0];
    const char *child_file  = argv[1];
    double at_x = 0, at_y = 0, w = 30, h = 20;
    int have_at = 0;
    const char *pins_csv = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf", &at_x, &at_y) != 2) {
                fprintf(stderr, "error: --at expects X,Y\n"); return 1;
            }
            have_at = 1;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf", &w, &h) != 2) {
                fprintf(stderr, "error: --size expects W,H\n"); return 1;
            }
        } else if (strcmp(argv[i], "--pins") == 0 && i + 1 < argc) {
            pins_csv = argv[++i];
        }
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    if (!have_at) {
        size_t idx = count_placed_symbols(root) + count_sheets(root);
        auto_slot(idx, &at_x, &at_y);
    }
    at_x = snap_grid(at_x, 1.27);
    at_y = snap_grid(at_y, 1.27);

    /* Resolve child path relative to the parent's directory. */
    char child_full[KICLI_PATH_MAX];
    if (child_file[0] == '/' || child_file[0] == '\\') {
        snprintf(child_full, sizeof(child_full), "%s", child_file);
    } else {
        char parent_dir[KICLI_PATH_MAX];
        snprintf(parent_dir, sizeof(parent_dir), "%s", sch_path);
        char *slash = strrchr(parent_dir, '/');
#ifdef _WIN32
        char *bslash = strrchr(parent_dir, '\\');
        if (bslash > slash) slash = bslash;
#endif
        if (slash) {
            *slash = '\0';
        } else {
            parent_dir[0] = '.';
            parent_dir[1] = '\0';
        }
        snprintf(child_full, sizeof(child_full), "%s/%s", parent_dir, child_file);
    }

    if (write_blank_child(child_full, sheet_name) != 0) {
        fprintf(stderr, "error: cannot create child '%s': %s\n",
                child_full, strerror(errno));
        sexpr_free(root); return 3;
    }

    /* Parse --pins CSV into a NULL-terminated array. */
    const char *pins_arr[64] = {0};
    char pins_buf[1024];
    if (pins_csv) {
        snprintf(pins_buf, sizeof(pins_buf), "%s", pins_csv);
        size_t n = 0;
        char *tok = strtok(pins_buf, ",");
        while (tok && n < 63) { pins_arr[n++] = tok; tok = strtok(NULL, ","); }
        pins_arr[n] = NULL;
    }

    const char *root_uuid = get_root_uuid(root);
    char proj[128];
    project_name_for(sch_path, proj, sizeof(proj));

    /* Sheetfile property is the child basename — KiCad resolves it relative
     * to the parent. */
    const char *cf_base = strrchr(child_file, '/');
#ifdef _WIN32
    const char *cf_b2 = strrchr(child_file, '\\');
    if (cf_b2 > cf_base) cf_base = cf_b2;
#endif
    cf_base = cf_base ? cf_base + 1 : child_file;

    sexpr_t *sheet = mk_sheet(sheet_name, cf_base, at_x, at_y, w, h,
                              root_uuid, proj, pins_arr[0] ? pins_arr : NULL);
    sexpr_list_append(root, sheet);

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    size_t pcount = 0; for (; pins_arr[pcount]; pcount++) ;
    printf("sheet '%s' → %s at %.2f,%.2f (size %.0fx%.0f, %zu pin(s))\n",
           sheet_name, child_full, at_x, at_y, w, h, pcount);
    if (pcount) {
        printf("  pins: ");
        for (size_t i = 0; i < pcount; i++)
            printf("%s%s", pins_arr[i], i + 1 < pcount ? ", " : "\n");
    }

    sexpr_free(root);
    return 0;
}
