/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/generic/msi.h>


MSIInterface* sMSIInterface;


void
msi_set_interface(MSIInterface* interface)
{
	sMSIInterface = interface;
}


bool
msi_supported()
{
	return sMSIInterface != NULL;
}


status_t
msi_allocate_vectors(uint32 count, uint32 *startVector, uint64 *address, uint32 *data)
{
	return sMSIInterface->AllocateVectors(count, *startVector, *address, *data);
}


status_t
msi_allocate_vectors_for_device(uint32 requesterID, uint32 count,
	uint32 *startVector, uint64 *address, uint32 *data)
{
	return sMSIInterface->AllocateVectors(requesterID, count, *startVector,
		*address, *data);
}


void
msi_free_vectors(uint32 count, uint32 startVector)
{
	sMSIInterface->FreeVectors(count, startVector);
}
