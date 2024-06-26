alias fll='fileutil ls -l -a -h -F -sharded -summary'
alias fls='fileutil ls -h -F -sharded -summary'
alias fmv='fileutil mv -sharded'
alias frm='fileutil rm -sharded'
alias fdel='fileutil recursivedeletegfsbackup'
alias fdu='fileutil du -h -sharded'
alias fcat='fileutil cat -a'
alias ftee='fileutil tee'
alias fcp='fileutil cp -R -a -f'
alias fmkdir='fileutil mkdir -p'
alias frmdir='fileutil rmdir'
alias cclean='/google/src/head/depot/google3/devtools/maintenance/cclean/cclean'
alias include-cleaner='/google/bin/releases/lpt-c-tools/include-cleaner/include_cleaner'


alias g='gqui'
alias count='gqui "select count(*)" from'
alias -g gfv='proto research_scam.GenericFeatureVector'
alias -g nns='proto research_scam.NearestNeighbors'
alias -g tfe='proto tensorflow.Example'
alias -g elwc='proto tensorflow.serving.ExampleListWithContext'
alias -g tfs='proto tensorflow.SequenceExample'
alias -g lestore='proto quality_local_ranking.LocalembedStore'
alias -g cout='proto research_cluston.ClustonOutput'
alias -g ypd='proto quality_local_ranking.LocalYpData'
alias -g ypr='proto quality_local_ranking.LocalYpDataRecord'

alias print_artemis_doc='/google/bin/releases/localweb-indexing/public/print_artemis_doc'
alias jarvis_cli='/google/data/ro/teams/ke-graph-exp/tools/jarvis_cli'
alias demo_hosted='/google/bin/releases/search-quality-eval-hosted/demo_hosted'
alias eval_hosted='/google/bin/releases/search-quality-eval-hosted/eval_hosted'
alias bgrep='/google/data/ro/teams/borgtools/bgrep'
alias bkill='/google/data/ro/teams/borgtools/bkill'
alias build_cleaner='build_cleaner'
alias build='blaze build -c opt'
alias run='blaze run -c opt'
alias spiffy='/google/data/ro/projects/superroot/spiffy'
alias spiffy_sync='/google/data/ro/projects/superroot/spiffy_sync'
alias servo='/google/data/ro/teams/ml-serving/mpm/servomatic_cli/servomatic_default/servo'
alias bagua_runner='/google/data/ro/teams/bagua/bagua_runner'
alias gpaste='/google/src/head/depot/eng/tools/pastebin'
alias -g FL="--cfs_d_cid_enable --cfs_d_resource_user_override=geo-search-signals-batch-jobs --flume_batch_scheduler_strategy=RUN_USING_WEAKLY_AVAILABLE --flume_borg_accounting_charged_user_name=geo-search-signals-batch-jobs --flume_borg_cells=-ql:-om:-qk:-iq --flume_dax_reexecution_mode=RELAXED --noflume_dax_use_single_worker_pool --flume_enable_backups --flume_enable_public_scratch --flume_guaranteed_batch_duration_secs=3600 --flume_use_batch_scheduler"
alias mid_tool=/google/data/rw/teams/livegraph/tools/mid_tool
alias cs='cs --local --nostats'
alias aclcheck=/google/data/ro/projects/ganpati/aclcheck

function csf() {
  cs f:$@
}


if [[ -n $PROD_PROXY ]]
then
  alias fileutil='noglob ras fileutil'
  alias gqui='noglob ras -t gqui'

  alias g4='ras -t g4'
  alias p4='ras -t p4'
  alias cs="ras -t $aliases[cs]"
  alias hb='ras -t hb'
  alias rrg='ras -t rg'
  alias blaze='ras -t blaze'
  alias borgcfg='ras -t borgcfg'
  alias citctool='ras citctools'
  alias build_cleaner='ras build_cleaner'
  alias rabbit='ras -t rabbit'
  alias placer='ras -t placer'
  alias borg='ras -t borg'
  alias mendel='ras -t mendel'
  alias effingo='ras -t effingo'

  alias rfind='ras find'
  alias rgrep='ras grep'
  alias rvim='ras -t vim'
  alias rless='ras -t less'
  alias rmv='ras mv -iv'
  alias rcp='ras cp -ivdRp'
  alias rscp='ras rsync -azPe "ssh -q"'
  alias rsh='ras -t zsh'
  alias print_artemis_doc="ras -t $aliases[print_artemis_doc]"
  alias jarvis_cli="noglob ras -t $aliases[jarvis_cli]"
  alias demo_hosted="noglob ras -t $aliases[demo_hosted]"
  alias eval_hosted="noglob ras -t $aliases[eval_hosted]"
  alias bgrep="noglob ras $aliases[bgrep]"
  alias bkill="noglob ras $aliases[bkill]"
  alias build_cleaner="ras -t $aliases[build_cleaner]"
  alias spiffy="ras -t $aliases[spiffy]"
  alias spiffy_sync="ras -t $aliases[spiffy_sync]"
  alias servo="ras -t $aliases[servo]"
fi

# The command used to generate an sstable.
how() {
  gqui describe "$1" | sed -n 's/.*command_line:\s*[^ ]*\///;s/ --/\n  --/gp' | sed '/--dax|--undefok|--streamz|--bigtable|--spanner|--binarylog|eventmanager/d'
}
