.. _smart_toilet_app:

Smart Toilet — voice-triggered flush
####################################

.. contents::
   :local:
   :depth: 2

The **Smart Toilet** application is a voice-activated flush actuator for the nRF54LM20 DK.
It detects the magic word **"Abracadabra"** from a digital microphone stream using |EAILib| and Axon-based inference, and on detection drives a flush motor through one rotation.

All inference runs on-device on the Axon AI accelerator; no audio leaves the board.

Application overview
********************

The application samples single-channel 16 kHz audio from a PDM microphone, cleans it up with a front-end audio chain, and feeds it to an nRF Edge AI wake-word model.
By default it runs in **wake-word-only mode** (``APP_MODE_WW_ONLY``): it listens continuously for the single phrase "Abracadabra" and never switches to a keyword-spotting stage.

When the wake word is detected, the application:

* blinks **LED0** for one second,
* prints ``Wakeword detected`` on the control UART (VCOM0),
* runs the flush motor through one rotation and stops it at the home position using a Hall sensor.

Model output is post-processed with a predictions-history window. The detection parameters are the per-frame probability threshold, the predictions-history length, and the number of predictions above the threshold required within that window. A short refractory period ensures a single utterance fires exactly once.

.. note::

   The application also supports wakeword-gated keyword spotting and keyword-spotting-only modes through the ``APP_MODE`` Kconfig choice, and bundles a keyword-spotting model (Go, Stop, Up, Down, Yes, No, On, Off, Right, Left). These are not used by the deployed Smart Toilet, which is wake-word-only.

Wake word
=========

The active wake word is selected by the ``APP_WW_MODEL`` Kconfig choice:

* ``APP_WW_MODEL_ABRACADABRA`` (default) — "Abracadabra", a 5-syllable, plosive-rich phrase from nRF Edge AI Lab. ~95–100% detection at far-field in live testing.
* ``APP_WW_MODEL_OKAY_NORDIC`` — "Okay Nordic", the add-on's bundled 4-syllable model, kept as a known-good reference (~85–90% far-field).

Longer, plosive-rich words survive far-field room reverb much better. A short fricative-led word loses exactly the high-frequency energy that reverb destroys, so it collapses at distance.

Audio front-end
===============

Each audio block is processed in :file:`src/audio_proc.c` before detection, in this order:

#. **PDM hardware gain** — ``APP_PDM_GAIN_DB`` (default +20 dB) applied in the PDM peripheral, sized for far-field use.
#. **High-pass filter** — ``APP_AUDIO_HPF``, a 2nd-order Butterworth high-pass at 120 Hz that strips rumble and handling noise below the speech band.
#. **Automatic gain control (AGC)** — ``APP_AGC`` tracks the speech peak envelope and applies a smoothed software gain toward ``APP_AGC_TARGET_DBFS`` (-20 dBFS), making detection less sensitive to speaker distance. It is gated below -38 dBFS so it rides utterance peaks instead of amplifying room noise.

The AGC acts after the PDM hardware gain and cannot undo saturation in the peripheral itself, so keep ``APP_PDM_GAIN_DB`` low enough that close or loud speech does not clip.

Replacing the model
===================

You can replace the wake-word model using the `Text to Wake Word Detection <Nordic Edge AI Lab Wake Word Detection_>`_ feature of the `Nordic Edge AI Lab`_ or one of the `ready-to-use models <Nordic Edge AI Lab ready-to-use models_>`_:

#. Add the generated files under a new directory in :file:`src/ww/models/`.
#. Add a matching option to the ``APP_WW_MODEL`` Kconfig choice and point :c:func:`ww_init` at the new model instance.
#. Adjust the ``WW_*`` Kconfig options to tune detection post-processing for your model.

Requirements
************

The application supports the following development kit:

.. table-from-sample-yaml::

It also requires a PDM digital microphone connected to the pins specified in the :file:`boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay` file, and the flush motor and Hall sensor described under `Pin mapping`_. Audio is expected on the left channel.

Pin mapping
===========

The application was tested with an `Adafruit PDM MEMS Microphone`_ module, powered from the 1.8V ``VDD:IO`` supply.

.. list-table::
   :header-rows: 1

   * - Signal
     - nRF54LM20 DK pin
     - Notes
   * - Motor drive
     - ``P1.06``
     - Active-high, drives a logic-level MOSFET gate
   * - Hall sensor
     - ``P1.07``
     - Active-low with internal pull-up; power the sensor from 5V
   * - Microphone ``CLK``
     - ``P1.04``
     - Adafruit PDM MEMS microphone
   * - Microphone ``DAT``
     - ``P1.05``
     - Microphone ``SEL`` → ``GND`` selects the left channel; ``VDD`` → ``VDD:IO`` (1.8V)

.. note::

   Do not use ``P2.00``–``P2.05`` as header GPIO on this DK: the board controller mux routes them to the onboard QSPI flash, so the header pins are inactive by default. Use ``P1``/``P3`` GPIO instead.

To use other microphones, adapt the PDM configuration parameters in the :file:`src/dmic.c` file.

User interface
**************

LED0:
   Blinks for one second when the wake word is detected (wake-word-only mode).

LED1:
   Blinks for one second on each keyword spotted (keyword-spotting modes only).

Flush motor:
   Runs through one rotation on wake-word detection and stops at the home position via the Hall sensor.

UART30 (VCOM0):
   Prints runtime state messages:

   * ``Waiting for wakeword``
   * ``Wakeword detected``

UART (VCOM1):
   Zephyr log output, including actuator events and, when ``APP_AUDIO_STATS`` is enabled, per-second microphone level, AGC gain, and wake-word probability statistics for tuning.

Configuration
*************

|config|

Configuration options
=====================

|application_kconfig|

.. options-from-kconfig::
   :show-type:

Building and running
********************

.. |application path| replace:: :file:`app`

This application is built against the nRF Edge AI add-on. See the repository
``README.md`` for the full ``west build`` and ``west flash`` command lines.

Testing
=======

|test_application|

#. |connect_kit|
#. |connect_terminal_kit| The application uses both serial ports.
#. Open one terminal for the control output from UART30 (VCOM0) and one for the Zephyr log (VCOM1).
#. Say the magic word "Abracadabra" from the normal use position.
#. Observe **LED0** blink for one second and the flush motor run through one rotation.
#. Observe ``Wakeword detected`` on VCOM0.

Application output
==================

The application shows the following output from UART30 (VCOM0):

.. code-block:: console

   Waiting for wakeword
   Wakeword detected

Dependencies
************

This application uses the following |EAI| library:

* :ref:`nrf_edgeai_lib`

This application uses the following Zephyr libraries:

* `Logging`_
* Audio (DMIC)
* GPIO
* UART Driver

API documentation
*****************

.. doxygengroup:: ww_kws
