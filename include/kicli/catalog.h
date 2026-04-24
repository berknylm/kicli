/*
 * catalog.h — symbol & footprint library discovery
 *
 * Browsing surface agents need before picking a Footprint or lib_id:
 *   kicli sym search <q> [--pins N] [--lib L] [-n N]
 *   kicli sym info   <lib:name>
 *   kicli sym list   [lib]
 *   kicli fp  search <q> [--pads N] [--lib L] [-n N]
 *   kicli fp  info   <lib:name>
 *   kicli fp  list   [lib]
 *
 * Sources scanned (in order):
 *   1. KiCad bundled shared libraries (<SharedSupport>/symbols, /footprints)
 *   2. Current project's libs/symbols and libs/footprints (.pretty folders)
 *      if --project given or a .kicad_pro is detected in cwd/ancestors.
 *   3. User's globally-registered libs from sym-lib-table / fp-lib-table are
 *      NOT scanned in this first release — scope bounded to the two most
 *      common sources. Covers SnapEDA imports + default KiCad inventory.
 */

#ifndef KICLI_CATALOG_H
#define KICLI_CATALOG_H

int cmd_sym(int argc, char **argv);
int cmd_fp (int argc, char **argv);

#endif /* KICLI_CATALOG_H */
