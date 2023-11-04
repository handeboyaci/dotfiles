
# Copyright (C) 2012 Anaconda, Inc
# SPDX-License-Identifier: BSD-3-Clause
export MAMBA_ROOT_PREFIX=$HOME/.conda

__mamba_exe() (
    "$HOME/.dotfiles/bin/conda" "${@}"
)

__mamba_hashr() {
      builtin rehash
}

__mamba_xctivate() {
    builtin local ask_conda
    ask_conda="$(PS1="${PS1:-}" __mamba_exe shell "${@}" --shell bash)" || builtin return
    builtin eval "${ask_conda}"
    __mamba_hashr
}

conda() {
    builtin local cmd="${1-__missing__}"
    case "${cmd}" in
        activate|reactivate|deactivate)
            __mamba_xctivate "${@}"
            ;;
        install|update|upgrade|remove|uninstall)
            __mamba_exe "${@}" || builtin return
            __mamba_xctivate reactivate
            ;;
        self-update)
            __mamba_exe "${@}" || builtin return

            # remove leftover backup file on Windows
            if [ -f "$HOME/.dotfiles/bin/conda.bkup" ]; then
                rm -f "$HOME/.dotfiles/bin/conda.bkup"
            fi
            ;;
        *)
            __mamba_exe "${@}"
            ;;
    esac
}

if [ -z "${CONDA_SHLVL+x}" ]; then
    builtin export CONDA_SHLVL=0
    builtin export PATH="${MAMBA_ROOT_PREFIX}/condabin:${PATH}"
    # We're not allowing PS1 to be unbound. It must at least be set.
    # However, we're not exporting it, which can cause problems when starting a second shell
    # via a first shell (i.e. starting zsh from bash).
    if [ -z "${PS1+x}" ]; then
        PS1=
    fi
fi
