# RTX Round-Robin Multitasking Lab
## Overview

This project was developed as part of COE718: Embedded Systems Design at Toronto Metropolitan University. The objective of the lab is to demonstrate multitasking in an embedded environment using Keil RTX (a Real-Time Operating System) on the LPC1768 ARM Cortex-M3 microcontroller.

We were introduced to the fundamentals of round-robin scheduling, thread management, and task synchronization using RTX APIs. The lab demonstrates the behavior of multiple concurrent threads executing under fixed time slices and provides insights into task switching, CPU utilization, and real-time performance analysis using uVision’s debugging tools.

## Objectives

- Implement multithreading using RTX with round-robin scheduling.

- Configure RTX system parameters such as SysTick, kernel timer, and time slice.

- Create and manage multiple threads using the CMSIS-RTOS API.

- Observe task execution patterns with the Event Viewer, Performance Analyzer, and RTX System Viewer.

- Understand CPU idle time, task priorities, and thread management in a real-time system.

## Features

- **Three-thread Round-Robin Scheduler:** Each thread executes within a fixed 15 ms time slice.

- **Configurable RTX Kernel:** Custom kernel tick rate and user timers defined in RTX_Conf_CM.c.

- **Event Visualization:** Thread execution monitored using the Keil Event Viewer and Performance Analyzer.

- **Idle Task Measurement:** Integrated idle loop (os_idle_demon) to measure CPU utilization.

### Two Build Versions:

**Analysis Version:** Debug-focused, without peripherals.

**Demo Version:** Includes LED and LCD integration for real-time visual feedback.

## Concepts Covered

- Real-Time Operating Systems (RTOS) basics

- Round-Robin scheduling and context switching

- Thread creation and management with CMSIS-RTOS API

- System tick timer configuration and timing analysis

- Task synchronization and inter-thread signaling

- CPU utilization and idle-time measurement

## Tools and Environment

**IDE:** Keil uVision 5

**Target MCU:** NXP LPC1768 (ARM Cortex-M3)

**RTOS:** Keil RTX (CMSIS-RTOS)

**Debugger Tools:** Event Viewer, Performance Analyzer, RTX System and Thread Viewer Watch Windows
