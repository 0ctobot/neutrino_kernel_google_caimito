#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

exec tools/bazel run \
    --config=caimito \
    --config=fast \
    --config=pixel_debug_common \
    --config=aosp_staging \
    //private/devices/google/caimito:zumapro_caimito_dist "$@"
