#
# Copyright (c) 2023 Andreas Sandberg
#
# SPDX-License-Identifier: Apache-2.0
#

board_runner_args(stm32cubeprogrammer "--port=swd")
board_runner_args(jlink "--device=STM32G473VB" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
