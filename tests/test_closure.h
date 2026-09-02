/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_TEST_CLOSURE_H
#define KOBOX2_TEST_CLOSURE_H

#include <kobox2/controller.h>

#include <stdint.h>

int kb2_test_configure_closure(kb2_controller_t *controller);
int kb2_test_configure_closure_with_reset(kb2_controller_t *controller, int reset_required);
int kb2_test_configure_fixture_closure(kb2_controller_t *controller,
                                       const uint8_t manifest_digest[KB2_DIGEST_SIZE],
                                       const uint8_t core_digest[KB2_DIGEST_SIZE],
                                       const uint8_t module_digest[KB2_DIGEST_SIZE]);

#endif
