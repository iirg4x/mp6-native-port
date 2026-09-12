/* Cross-link fixture: real AArch64 relocations, not a mocked linker. */
extern int puts(const char *);
int public_data = 7;
int strong_function(void) { return 11; }
__attribute__((weak)) int weak_function(void) { return 13; }
int (*strong_address(void))(void) { return strong_function; }
int (*weak_address(void))(void) { return weak_function; }
int *data_address(void) { return &public_data; }
int JNI_OnLoad(void *vm, void *reserved) { return vm != reserved; }
int probe(void) {
    return strong_function() + weak_function() + public_data + puts("fixture");
}
