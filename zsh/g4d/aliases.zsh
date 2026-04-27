alias -g FL="--cfs_d_cid_enable --cfs_d_resource_user_override=gem-embed-cns-accounting --flume_batch_scheduler_strategy=RUN_USING_WEAKLY_AVAILABLE --flume_borg_accounting_charged_user_name=lens-embedding-dev --flume_borg_cells=-ql:-om:-qk:-iq --flume_dax_reexecution_mode=RELAXED --flume_enable_backups --flume_enable_public_scratch --flume_guaranteed_batch_duration_secs=3600 --flume_resumable --flume_use_batch_scheduler --noflume_dax_use_single_worker_pool"
alias -g elwc='proto tensorflow.serving.ExampleListWithContext'
alias -g gfv='proto research_scam.GenericFeatureVector'
alias -g nns='proto research_scam.NearestNeighbors'
alias -g tfe='proto tensorflow.Example'
alias -g tfs='proto tensorflow.SequenceExample'
alias aclcheck=/google/data/ro/projects/ganpati/aclcheck
alias bgrep='/google/data/ro/teams/borgtools/bgrep'
alias bkill='/google/data/ro/teams/borgtools/bkill'
alias build='blaze build -c opt'
alias cclean='/google/src/head/depot/google3/devtools/maintenance/cclean/cclean'
alias colab="/google/bin/releases/grp-ix-team/rapid/colab-cli/cli.par"
alias count='gqui "select count(*)" from'
alias cs='cs --local --nostats'
alias er='/google/data/ro/users/ho/hooper/er'
alias f='/google/data/ro/teams/tf-hub/fileutil'
alias fcat='f cat -a'
alias fcp='f cp -R -a -f'
alias fdel='f recursivedeletegfsbackup'
alias fdu='f du -h -sharded'
alias fll='f ls -l -a -h -F -sharded -summary'
alias fls='f ls -h -F -sharded -summary'
alias fmkdir='f mkdir -p'
alias fmv='f mv -sharded'
alias frm='f rm -sharded'
alias frmdir='f rmdir'
alias ftee='f tee'
alias g='gqui'
alias gemini='/google/bin/releases/gemini-cli/tools/gemini'
alias gpaste='/google/src/head/depot/eng/tools/pastebin'
alias include-cleaner='/google/bin/releases/lpt-c-tools/include-cleaner/include_cleaner'
alias jarvis_cli='/google/data/ro/teams/ke-graph-exp/tools/jarvis_cli'
alias jarvis='/google/bin/releases/jarvis-cli/jarvis'
alias jetski='/google/bin/releases/jetski-devs/tools/cli'
alias lp-run='/google/src/head/depot/google3/third_party/py/launchpad/scripts/lp-run.sh'
alias mid_tool=/google/data/rw/teams/livegraph/tools/mid_tool
alias run='blaze run -c opt'
alias servo='/google/data/ro/teams/ml-serving/mpm/servomatic_cli/servomatic_default/servo'
alias sharemymodel=/google/bin/releases/airr-saif-data/sharemymodel/sharemymodel
alias xmanager='/google/bin/releases/xmanager/cli/xmanager.par'

function csf() {
  cs f:$@
}


if [[ -n $PROD_PROXY ]]
then

  alias bgrep="noglob ras $aliases[bgrep]"
  alias bkill="noglob ras $aliases[bkill]"
  alias blaze='ras -t blaze'
  alias borg='ras -t borg'
  alias borgcfg='ras -t borgcfg'
  alias build_cleaner='ras build_cleaner'
  alias citctool='ras citctools'
  alias cs="ras -t $aliases[cs]"
  alias effingo='ras -t effingo'
  alias fileutil='noglob ras fileutil'
  alias g4='ras -t g4'
  alias gqui='noglob ras -t gqui'
  alias hb='ras -t hb'
  alias jarvis_cli="noglob ras -t $aliases[jarvis_cli]"
  alias mendel='ras -t mendel'
  alias p4='ras -t p4'
  alias placer='ras -t placer'
  alias print_artemis_doc="ras -t $aliases[print_artemis_doc]"
  alias rabbit='ras -t rabbit'
  alias rcp='ras cp -ivdRp'
  alias rfind='ras find'
  alias rgrep='ras grep'
  alias rless='ras -t less'
  alias rmv='ras mv -iv'
  alias rrg='ras -t rg'
  alias rscp='ras rsync -azPe "ssh -q"'
  alias rsh='ras -t zsh'
  alias rvim='ras -t vim'
  alias servo="ras -t $aliases[servo]"
  alias spiffy="ras -t $aliases[spiffy]"
  alias spiffy_sync="ras -t $aliases[spiffy_sync]"
  alias xmanager="ras -t $aliases[xmanager]"
fi

# The command used to generate an sstable.
how() {
  gqui describe "$1" | sed -n 's/.*command_line:\s*[^ ]*\///;s/ --/\n  --/gp' | sed '/--dax|--undefok|--streamz|--bigtable|--spanner|--binarylog|eventmanager/d'
}
