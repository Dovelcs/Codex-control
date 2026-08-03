# STM32CubeG0 driver subset

This directory contains the CMSIS headers and STM32G0 HAL sources required by
this firmware. They were copied from STMicroelectronics/STM32CubeG0 commit
`b1d88f9e17290d5c5328399bb02bb5ef82deb03a`.

The upstream license files are retained in the corresponding CMSIS and HAL
directories. Only the HAL source files linked by the CubeIDE project are
included; all HAL headers are retained so optional peripheral headers continue
to resolve correctly.
