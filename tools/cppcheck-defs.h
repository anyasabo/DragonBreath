/* Included only by cppcheck (--include). cppcheck cannot parse a GNU asm label on a
 * declaration, for example
 *   extern const unsigned char favicon_png_start[] asm("_binary_favicon_png_start");
 * Without the label, the declaration is a normal extern array. */
#define asm(name)
