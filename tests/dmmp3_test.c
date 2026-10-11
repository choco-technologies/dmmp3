#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmmp3.h"

static dmmp3_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmmp3_create();
}

void dmod_test_teardown(void)
{
    dmmp3_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmmp3_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmmp3_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmmp3_is_valid(g_handle));
}

DMOD_TEST_STEP(dmmp3_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmmp3_destroy(NULL);
}
