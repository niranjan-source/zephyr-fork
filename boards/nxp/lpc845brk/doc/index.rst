.. zephyr:board:: lpc845brk

Overview
********

The LPC845 Breakout Board, developed by NXP, enables rapid evaluation and
prototyping with the LPC84x family of microcontrollers.

The LPC84x family is based on the Arm® Cortex®-M0+ architecture and is a
low-cost 32-bit MCU family operating at CPU frequencies of up to 30 MHz.
It supports up to 64 KB of Flash memory and 16 KB of SRAM, along with a
flexible peripheral configuration using a switch matrix.

Hardware
********

The LPC845-BRK board is based on the NXP LPC845 MCU and provides:

- Arm Cortex-M0+ CPU running at up to 30 MHz
- 64 KB on-chip Flash and 16 KB SRAM
- Internal Free Running Oscillator (FRO) with multiple frequency options
- 5x USART interfaces with switch matrix routing and fractional baud rate
- 2x SPI controllers with switch matrix routing
- 4x I2C interfaces (one supporting Fast-mode Plus at 1 Mbit/s)
- One 12-bit ADC and two 10-bit DACs
- SCTimer/PWM and multiple general-purpose timers
- 25-channel DMA controller and CRC engine
- Up to 54 GPIOs with flexible switch matrix configuration
- Pin interrupts and Pattern match engine support on up to eight pins (irq slots)
- Serial Wire Debug (SWD) interface
- Capacitive Touch Interface


.. zephyr:board-supported-hw::

Connections and IOs
===================

The LPC845 uses a switch matrix and IOCON block to configure pin functions.

Default pin configuration:

+---------+-----------------+-------------------+
| Name    | Function        | Usage             |
+=========+=================+===================+
| PIO0_24 | UART            | USART0 TX         |
+---------+-----------------+-------------------+
| PIO0_25 | UART            | USART0 RX         |
+---------+-----------------+-------------------+
| PIO1_0  | GPIO            | GREEN LED         |
+---------+-----------------+-------------------+
| PIO1_1  | GPIO            | BLUE LED          |
+---------+-----------------+-------------------+
| PIO1_2  | GPIO            | RED LED           |
+---------+-----------------+-------------------+

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Building
========

Here is an example for building the :zephyr:code-sample:`hello_world` sample application.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: lpc845brk
   :goals: build

Flashing
========

Once the application is built, you can flash it using:

.. code-block:: console

   west flash

Debugging
=========

To debug the application:

.. code-block:: console

   west debug

References
**********

- `NXP LPC845 Breakout Board Website`_
- `LPC84X Data Sheet`_
- `LPC845 BRK User Manual`_

.. include:: ../../common/board-footer.rst.inc

.. _NXP LPC845 Breakout Board Website:
   https://www.nxp.com/design/design-center/development-boards-and-designs/LPC845-BRK

.. _LPC845 BRK User Manual:
   https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/lpc/35244/1/UM11181.pdf

.. _LPC84X Data Sheet:
   https://www.nxp.com/docs/en/data-sheet/LPC84x.pdf
