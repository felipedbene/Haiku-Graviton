// Issue 958: net_buffer field offsets and size, compiled with the stack
// add-on's own compile line. Each value appears in the assembly as an
// immediate operand on an "OFF <field> <offset> <size>" comment.
#include <SupportDefs.h>
#include <net_buffer.h>
#include <stddef.h>

#define F(f) asm volatile("// OFF " #f " %0 %1" :: "i"(offsetof(net_buffer, f)), \
	"i"(sizeof(((net_buffer*)0)->f)))

extern "C" void
net_buffer_offsets()
{
	F(link); F(source); F(destination); F(interface_address); F(fragment);
	F(sequence); F(offset); F(index); F(type); F(msg_flags); F(size);
	F(protocol);
#ifdef HAS_HOP
	F(hop_limit);
#endif
	F(buffer_flags); F(receive_enqueue_time);
	asm volatile("// SIZEOF net_buffer %0 align %1" :: "i"(sizeof(net_buffer)),
		"i"(alignof(net_buffer)));
}
