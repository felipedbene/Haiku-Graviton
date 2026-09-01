/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <TestSuiteAddon.h>
#include <cppunit/extensions/HelperMacros.h>

#include <algorithm>
#include <vector>

#include <util/MinMaxHeap.h>


/*	Regression cover for issue #114.

	MinMaxHeap used to expose PeekMinimum(int32 index)/PeekMaximum(int32 index),
	whose names promised the index-th smallest/largest element. They did not
	deliver: the backing arrays are heaps, so slot 0 is the true extremum but
	slot 1 is merely a child of the root, not the runner-up. The indexed form is
	now spelled PeekUnordered() and documents itself as an unordered but
	complete enumeration.

	These tests pin both halves of that contract, so nobody re-introduces an
	ordering promise the structure cannot keep:
	  - PeekMinimum()/PeekMaximum() (no index) ARE the true extrema;
	  - PeekUnordered() visits every element exactly once and NULLs past the
	    end, but is explicitly NOT in key order.
*/

namespace {

class Entry : public MinMaxHeapLinkImpl<Entry, int32> {
public:
	Entry()
		:
		fValue(0)
	{
	}

	void SetValue(int32 value) { fValue = value; }
	int32 Value() const { return fValue; }

private:
	int32 fValue;
};


typedef MinMaxHeap<Entry, int32> TestHeap;


// A heap plus stable storage for its elements. MinMaxHeap keeps raw pointers,
// so the backing array must be sized before anything is inserted.
class Fixture {
public:
	Fixture(size_t count)
		:
		fEntries(count)
	{
	}

	void Insert(size_t index, int32 key)
	{
		fEntries[index].SetValue(key);
		CPPUNIT_ASSERT_EQUAL(fHeap.Insert(&fEntries[index], key), (status_t)B_OK);
	}

	TestHeap& Heap() { return fHeap; }

private:
	std::vector<Entry> fEntries;
	TestHeap fHeap;
};


// Deterministic pseudo-random keys, so a failure is always reproducible.
static int32
NextKey(uint32& state)
{
	state = state * 1103515245 + 12345;
	return (int32)((state >> 16) % 1000);
}

} // namespace


class MinMaxHeapTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(MinMaxHeapTest);
	CPPUNIT_TEST(EmptyHeap_ReturnsNullAndZeroCount);
	CPPUNIT_TEST(PeekMinimum_ReturnsTrueMinimum);
	CPPUNIT_TEST(PeekMaximum_ReturnsTrueMaximum);
	CPPUNIT_TEST(CountElements_MatchesInsertedCount);
	CPPUNIT_TEST(PeekUnordered_VisitsEveryElementExactlyOnce);
	CPPUNIT_TEST(PeekUnordered_PastEnd_ReturnsNull);
	CPPUNIT_TEST(PeekUnordered_IsNotKeyOrder);
	CPPUNIT_TEST(ModifyKey_KeepsExtremaTrue);
	CPPUNIT_TEST(RemoveMinimum_KeepsExtremaTrue);
	CPPUNIT_TEST_SUITE_END();

public:
	static const int kMaxCount = 64;

	void EmptyHeap_ReturnsNullAndZeroCount()
	{
		TestHeap heap;

		CPPUNIT_ASSERT_EQUAL(heap.CountElements(), (int32)0);
		CPPUNIT_ASSERT(heap.PeekMinimum() == NULL);
		CPPUNIT_ASSERT(heap.PeekMaximum() == NULL);
		CPPUNIT_ASSERT(heap.PeekUnordered(0) == NULL);
	}

	void PeekMinimum_ReturnsTrueMinimum()
	{
		uint32 state = 12345;
		for (int n = 1; n <= kMaxCount; n++) {
			Fixture fixture(n);
			std::vector<int32> keys(n);
			for (int i = 0; i < n; i++) {
				keys[i] = NextKey(state);
				fixture.Insert(i, keys[i]);
			}

			const int32 expected = *std::min_element(keys.begin(), keys.end());
			Entry* minimum = fixture.Heap().PeekMinimum();
			CPPUNIT_ASSERT(minimum != NULL);
			CPPUNIT_ASSERT_EQUAL(TestHeap::GetKey(minimum), expected);
		}
	}

	void PeekMaximum_ReturnsTrueMaximum()
	{
		uint32 state = 4242;
		for (int n = 1; n <= kMaxCount; n++) {
			Fixture fixture(n);
			std::vector<int32> keys(n);
			for (int i = 0; i < n; i++) {
				keys[i] = NextKey(state);
				fixture.Insert(i, keys[i]);
			}

			const int32 expected = *std::max_element(keys.begin(), keys.end());
			Entry* maximum = fixture.Heap().PeekMaximum();
			CPPUNIT_ASSERT(maximum != NULL);
			CPPUNIT_ASSERT_EQUAL(TestHeap::GetKey(maximum), expected);
		}
	}

	void CountElements_MatchesInsertedCount()
	{
		uint32 state = 777;
		for (int n = 1; n <= kMaxCount; n++) {
			Fixture fixture(n);
			for (int i = 0; i < n; i++)
				fixture.Insert(i, NextKey(state));

			CPPUNIT_ASSERT_EQUAL(fixture.Heap().CountElements(), (int32)n);
		}
	}

	//! The guarantee PeekUnordered() does make: a complete enumeration.
	void PeekUnordered_VisitsEveryElementExactlyOnce()
	{
		uint32 state = 999;
		for (int n = 1; n <= kMaxCount; n++) {
			Fixture fixture(n);
			std::vector<int32> keys(n);
			for (int i = 0; i < n; i++) {
				keys[i] = NextKey(state);
				fixture.Insert(i, keys[i]);
			}

			std::vector<int32> enumerated;
			const int32 count = fixture.Heap().CountElements();
			for (int32 i = 0; i < count; i++) {
				Entry* entry = fixture.Heap().PeekUnordered(i);
				CPPUNIT_ASSERT(entry != NULL);
				enumerated.push_back(TestHeap::GetKey(entry));
			}

			std::sort(keys.begin(), keys.end());
			std::sort(enumerated.begin(), enumerated.end());
			CPPUNIT_ASSERT(enumerated == keys);
		}
	}

	void PeekUnordered_PastEnd_ReturnsNull()
	{
		uint32 state = 31337;
		for (int n = 1; n <= kMaxCount; n++) {
			Fixture fixture(n);
			for (int i = 0; i < n; i++)
				fixture.Insert(i, NextKey(state));

			CPPUNIT_ASSERT(fixture.Heap().PeekUnordered(n) == NULL);
			CPPUNIT_ASSERT(fixture.Heap().PeekUnordered(n + 1) == NULL);
		}
	}

	/*!	The guarantee PeekUnordered() does NOT make. This is issue #114: the
		enumeration is heap-array order, so slot 1 is not the runner-up. If a
		future change makes the enumeration sorted, this test fails loudly and
		whoever did it can decide deliberately whether to keep paying for it --
		rather than a caller silently assuming an order that was never there.
	*/
	void PeekUnordered_IsNotKeyOrder()
	{
		// Smallest witness found by exhaustive search over insertion orders.
		const int32 kInsertOrder[] = { 10, 30, 40, 50, 20 };
		const int n = 5;

		Fixture fixture(n);
		for (int i = 0; i < n; i++)
			fixture.Insert(i, kInsertOrder[i]);

		// Slot 0 is the true minimum...
		Entry* first = fixture.Heap().PeekUnordered(0);
		CPPUNIT_ASSERT(first != NULL);
		CPPUNIT_ASSERT_EQUAL(TestHeap::GetKey(first), (int32)10);
		CPPUNIT_ASSERT_EQUAL(TestHeap::GetKey(fixture.Heap().PeekMinimum()),
			(int32)10);

		// ...but slot 1 is NOT the second smallest (20); it is a child of the
		// root. This is exactly what the old PeekMinimum(1) promised and missed.
		Entry* second = fixture.Heap().PeekUnordered(1);
		CPPUNIT_ASSERT(second != NULL);
		CPPUNIT_ASSERT(TestHeap::GetKey(second) != (int32)20);
	}

	void ModifyKey_KeepsExtremaTrue()
	{
		uint32 state = 2026;
		const int n = 32;

		Fixture fixture(n);
		std::vector<int32> keys(n);
		for (int i = 0; i < n; i++) {
			keys[i] = NextKey(state);
			fixture.Insert(i, keys[i]);
		}

		// Drive every element to a fresh key, re-checking both extrema each
		// time; this exercises _MoveUp/_MoveDown/_ChangeTree.
		for (int i = 0; i < n; i++) {
			const int32 newKey = NextKey(state);
			Entry* entry = fixture.Heap().PeekUnordered(i);
			CPPUNIT_ASSERT(entry != NULL);

			const int32 oldKey = TestHeap::GetKey(entry);
			std::vector<int32>::iterator found
				= std::find(keys.begin(), keys.end(), oldKey);
			CPPUNIT_ASSERT(found != keys.end());
			*found = newKey;

			fixture.Heap().ModifyKey(entry, newKey);

			CPPUNIT_ASSERT_EQUAL(
				TestHeap::GetKey(fixture.Heap().PeekMinimum()),
				*std::min_element(keys.begin(), keys.end()));
			CPPUNIT_ASSERT_EQUAL(
				TestHeap::GetKey(fixture.Heap().PeekMaximum()),
				*std::max_element(keys.begin(), keys.end()));
		}
	}

	void RemoveMinimum_KeepsExtremaTrue()
	{
		uint32 state = 8080;
		const int n = 48;

		Fixture fixture(n);
		std::vector<int32> keys(n);
		for (int i = 0; i < n; i++) {
			keys[i] = NextKey(state);
			fixture.Insert(i, keys[i]);
		}
		std::sort(keys.begin(), keys.end());

		// Draining via RemoveMinimum() must yield ascending keys -- the ordered
		// walk PeekUnordered() cannot give you.
		for (int i = 0; i < n; i++) {
			CPPUNIT_ASSERT_EQUAL(fixture.Heap().CountElements(), (int32)(n - i));

			Entry* minimum = fixture.Heap().PeekMinimum();
			CPPUNIT_ASSERT(minimum != NULL);
			CPPUNIT_ASSERT_EQUAL(TestHeap::GetKey(minimum), keys[i]);
			CPPUNIT_ASSERT_EQUAL(
				TestHeap::GetKey(fixture.Heap().PeekMaximum()), keys[n - 1]);

			fixture.Heap().RemoveMinimum();
		}

		CPPUNIT_ASSERT_EQUAL(fixture.Heap().CountElements(), (int32)0);
		CPPUNIT_ASSERT(fixture.Heap().PeekMinimum() == NULL);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(MinMaxHeapTest, getTestSuiteName());
