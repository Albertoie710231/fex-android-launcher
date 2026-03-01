/* Empty shim — tests if patchelf DT_NEEDED alone breaks FEX openat */
__attribute__((constructor))
static void empty_init(void) {
    /* intentionally empty */
}
