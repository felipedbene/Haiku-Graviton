#ifdef __cplusplus
extern "C" {
#endif

// One line per device plus a count: what a boot log must always keep.
void pci_print_info();

// The full multi-line dump of every device, for the `pcirefresh` debugger
// command. Far too much output to emit at boot -- see pci_print_info().
void pci_print_info_verbose();

#ifdef __cplusplus
}
#endif
