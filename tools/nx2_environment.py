# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit launch controls consumed by the imported NX2 implementation.

Keep this list aligned with the getenv consumers in core, video_core,
shader_recompiler, audio_core and yuzu_cmd. A name in a source comment alone
does not make it a supported environment variable. Values are passed verbatim;
the emulator owns their interpretation, including presence-only switches.
"""

NX2_ENVIRONMENT_VARIABLES = (
    "EDEN_NX2_3D_METHODS",
    "EDEN_NX2_ASTC_BC1",
    "EDEN_NX2_ASTC_FLUSH",
    "EDEN_NX2_AUDIO_LEVEL",
    "EDEN_NX2_AUTO_STUB",
    "EDEN_NX2_BUFFER_DOWNLOADS",
    "EDEN_NX2_CB24_TRACE",
    "EDEN_NX2_CNTPCT_PAUSE",
    "EDEN_NX2_CRASH_DUMP",
    "EDEN_NX2_DUMP",
    "EDEN_NX2_FENCE_BATCH",
    "EDEN_NX2_FORCE_LOD0",
    "EDEN_NX2_FRAME_CAPTURE",
    "EDEN_NX2_FRAMES_AHEAD",
    "EDEN_NX2_GPU_PROFILE",
    "EDEN_NX2_HIDE_WINDOW",
    "EDEN_NX2_IDLE_SPIN_US",
    "EDEN_NX2_INLINE_DEFER",
    "EDEN_NX2_IPC_SUMMARY",
    "EDEN_NX2_IPC_TRACE",
    "EDEN_NX2_KEY_RECORD",
    "EDEN_NX2_KEY_SCRIPT",
    "EDEN_NX2_MACRO_COPY",
    "EDEN_NX2_MEM_TRACE",
    "EDEN_NX2_METHOD_TRACE",
    "EDEN_NX2_MME_DUMP",
    "EDEN_NX2_MME_JIT",
    "EDEN_NX2_MME_LOG",
    "EDEN_NX2_MME_MEMO",
    "EDEN_NX2_MME_MEMO_STATS",
    "EDEN_NX2_MME_RECORD",
    "EDEN_NX2_MME_RECORD_SKIP",
    "EDEN_NX2_PARK_SPLIT_METHODS",
    "EDEN_NX2_PB_CHECK",
    "EDEN_NX2_PB_DUMP",
    "EDEN_NX2_PIPELINE_DIFF",
    "EDEN_NX2_PULLER_TRACE",
    "EDEN_NX2_QMD_TRACE",
    "EDEN_NX2_QUERY_TRACE",
    "EDEN_NX2_REJECT",
    "EDEN_NX2_RT_TRACE",
    "EDEN_NX2_SAMPLE",
    "EDEN_NX2_SAMPLE_AT",
    "EDEN_NX2_SAMPLE_OUT",
    "EDEN_NX2_SAMPLE_SECONDS",
    "EDEN_NX2_SAMPLE_START",
    "EDEN_NX2_SEM_WATCH",
    "EDEN_NX2_SEM_WATCH_EMITS",
    "EDEN_NX2_SHADER_DUMP",
    "EDEN_NX2_SKIP_SAME",
    "EDEN_NX2_SKIP_VS",
    "EDEN_NX2_SPLIT",
    "EDEN_NX2_SPLIT_CHECK",
    "EDEN_NX2_SPLIT_SKIP",
    "EDEN_NX2_SPLIT_VERIFY",
    "EDEN_NX2_SVC_TRACE",
    "EDEN_NX2_TC_CRITICAL_MIB",
    "EDEN_NX2_TC_EXPECTED_MIB",
    "EDEN_NX2_TC_MIN_AGE",
    "EDEN_NX2_TENSOR",
    "EDEN_NX2_TEXTURE_DUMP",
    "EDEN_NX2_TRACE_AFTER_UPSCALE",
    "EDEN_NX2_TRACE_DST",
    "EDEN_NX2_TRACE_PS",
    "EDEN_NX2_TRACE_VS",
    "EDEN_NX2_TRANSCODE_THREADS",
    "EDEN_NX2_UPSCALE_SOURCE",
    "EDEN_NX2_UPSCALE_SOURCE_FORMAT",
    "EDEN_NX2_VDEC_DUMP",
    "EDEN_NX2_VSYNC_HZ",
    "EDEN_NX2_WINDOW_TRACE",
    "EDEN_NX2_WINDOW_TRACE_SKIP",
)


def systemd_nx2_environment(environment):
    """Return argv additions and supplied nonsecret controls for run.json.

    UnsetEnvironment is applied last by systemd, so unset only absent names:
    this removes stale user-manager values without deleting explicit overrides.
    An empty supplied value remains present (important for getenv checks).
    No shell or systemd property-list quoting is applied to --setenv values;
    each complete assignment is one subprocess argument.
    """
    supplied = {name: environment[name] for name in NX2_ENVIRONMENT_VARIABLES
                if name in environment}
    arguments = ["--setenv=" + name + "=" + value for name, value in supplied.items()]
    absent = [name for name in NX2_ENVIRONMENT_VARIABLES if name not in supplied]
    if absent:
        arguments.append("--property=UnsetEnvironment=" + " ".join(absent))
    return arguments, supplied
