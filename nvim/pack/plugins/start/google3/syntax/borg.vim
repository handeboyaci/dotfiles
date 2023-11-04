" Copyright 2005 Google Inc.
" All Rights Reserved.
" Author: plakal@google.com (Manoj Plakal)
"
" borg.vim: Vim syntax file for Borg config files.
"
" Borg Config Language links:
" http://wiki.corp.google.com/twiki/bin/view/Main/BorgConfig
" http://g3doc/borg/configs/g3doc/borgcfg-release-notes
" //depot/google3/configlang
" //depot/google3/borg/configs

" For version 5.x: Clear all syntax items
" For version 6.x: Quit when a syntax file was already loaded
if version < 600
  syntax clear
elseif exists("b:current_syntax")
  finish
endif

" Borgcfg is based on gcl; import all elements.
runtime! syntax/gcl.vim

" These are possible future keywords (from Marcel vL, via Ovidiu P).
syn keyword borgKeyword      mutable function

" Object definitions: service, job, package, allocation, template, cdd rule.
" Highlight both the keyword and the name of the defined object that
" follows the keyword. Except when they are preceded by a '.'
" which is the borg way of allowing attributes with these special names.
syn region  borgObjectDef    matchgroup=gclObjectKeyword
                             \ start="\<\.\@<!\(service\|job\|package\|allocation\|template\|cdd\)\(\s*=\)\@!\>"
                             \ matchgroup=gclObjectName
                             \ end="\<[A-Za-z_][A-Za-z0-9_\-]*\>"
                             \ oneline skipwhite keepend

" Sections within a Borg object. Note that not all sections can
" occur within all objects, but we punt on context-sensitivity
" for now since that would require us to parse Borg Config Language
" within the edit buffer. Keep both sections and attributes (below)
" in sync with `grep -- '->Get' google3/borg/configs/*.cc.
" TODO(plakal): perhaps have a shell command to extract a list of
" candidate names from the source to generate this vimscript section.
syn keyword borgCfgSection accounting appclass args backend_info backends
syn keyword borgCfgSection canaries cdd_rules checkpoint_restore_policy
syn keyword borgCfgSection dependencies env_vars failover_class healthz_ports
syn keyword borgCfgSection job_health_params monitor monitoring options packages
syn keyword borgCfgSection permissions requirements resources restricted runtime
syn keyword borgCfgSection scheduling task_args tech_labels update
syn keyword borgCfgSection virtual_networking

" Sections within the 'cdd' object within 'cdd_rules' section.
syn keyword borgCfgSection baseline source

" Section within the 'requirements' section
syn keyword borgCfgSection autopilot_params

" Section within the 'scheduling' section
syn keyword borgCfgSection batch_quota colocation regional storage_colocation

" Attributes within the 'scheduling.storage_colocation' section
syn keyword borgCfgAttribute files

" Attributes within the 'scheduling.regional' section
syn keyword borgCfgAttribute allowed_cells close_to_resources forbidden_cells
syn keyword borgCfgAttribute ignore_configpkg_constraints

" Attributes within sections. Note that not all attributes can
" occur within all sections, but we punt on context-sensitivity.
syn keyword borgCfgAttribute account_child_mem_to_alloc acl after
syn keyword borgCfgAttribute aggressive_avoid_machine alloc
syn keyword borgCfgAttribute allow_checkpoint_restore
syn keyword borgCfgAttribute alloc_health_timeout_ms allow_auto_update
syn keyword borgCfgAttribute append_checksum argos attr_limits autopar_extract
syn keyword borgCfgAttribute autopilot autonomous_task_restart
syn keyword borgCfgAttribute autorestart_timeout binary
syn keyword borgCfgAttribute bcid_break_glass_justification
syn keyword borgCfgAttribute binary_package_names binary_version block_on_delete
syn keyword borgCfgAttribute borgletparams borg_package_type cached cavium_hsm
syn keyword borgCfgAttribute cell charged_user check_seconds children
syn keyword borgCfgAttribute chroot chubby_dir cleanup client clique_ssd
syn keyword borgCfgAttribute clique_pd_ssd colossus_cell command constraints cpu
syn keyword borgCfgAttribute cpu_platform
syn keyword borgCfgAttribute critical_jobs_present critical_task_grace_period
syn keyword borgCfgAttribute criticaljobs daemon data_version dataversion
syn keyword borgCfgAttribute deadline default_url degradation dionysus
syn keyword borgCfgAttribute directory disk
syn keyword borgCfgAttribute enable_diskless enforce_limits exclusive
syn keyword borgCfgAttribute external_params file fileset fixed_cpu fixed_disk
syn keyword borgCfgAttribute fixed_replicas cpu_utilization autopilot_for_java
syn keyword borgCfgAttribute fixed_ram gfs_cell global globaldir gneiss gpu
syn keyword borgCfgAttribute gpu_navi12_8gib_16th gpu_tesla_p100 gpu_tesla_p4
syn keyword borgCfgAttribute gpu_tesla_t4 gpu_tesla_v100 gpu_vega10_8gib_16th
syn keyword borgCfgAttribute group health_timeout_seconds healthz_timeout_ms
syn keyword borgCfgAttribute host_ipv4 hostname hugetlbfs_1g hugetlbfs_2m
syn keyword borgCfgAttribute install install_timeout jellydonut jellyfish
syn keyword borgCfgAttribute labelacl level local_ram_fs_dir
syn keyword borgCfgAttribute local_ssd_partition_fs_dir lockservice locus_type
syn keyword borgCfgAttribute machine_reservation_id managed_dirs master
syn keyword borgCfgAttribute master_port max_consecutive_failures
syn keyword borgCfgAttribute max_dead_tasks max_diskused_fraction
syn keyword borgCfgAttribute max_failing_tasks max_fraction_in_flight
syn keyword borgCfgAttribute max_in_flight max_initial_failures
syn keyword borgCfgAttribute max_initial_scheduling_wait
syn keyword borgCfgAttribute max_job_failures max_pending_tasks
syn keyword borgCfgAttribute max_pending_tasks_fraction max_per_rack
syn keyword borgCfgAttribute max_per_task_failures max_reschedules
syn keyword borgCfgAttribute max_restarts_per_hour_hint max_task_failures
syn keyword borgCfgAttribute max_tolerated_failures max_unhealthy_tasks
syn keyword borgCfgAttribute milligcu min_disks min_ms_since_last_failure
syn keyword borgCfgAttribute min_seconds_since_last_failure minimum_size
syn keyword borgCfgAttribute mode mpm_dir name nameservice_type ns_params
syn keyword borgCfgAttribute need_different_machines network_rate notify
syn keyword borgCfgAttribute num_canaries num_conns num_disks num_shards
syn keyword borgCfgAttribute numa_cpu_millicores numa_cpu_milligcu numa_memory
syn keyword borgCfgAttribute p4file package_binary parent
syn keyword borgCfgAttribute pathvar per_task pin_tasks_per_alloc_index
syn keyword borgCfgAttribute pinned ponyexpress portname ports post_install
syn keyword borgCfgAttribute preemption_notice preload_packages primary_eligible
syn keyword borgCfgAttribute primary_fs_dir_name priority
syn keyword borgCfgAttribute production_package_verification protocol pufferfish
syn keyword borgCfgAttribute puffylite ram rate_limit_ms raw_hpn_bandwidth
syn keyword borgCfgAttribute raw_hdd_devices raw_size raw_ssds
syn keyword borgCfgAttribute raw_ssd_partitions remote_hdd_fs_dir
syn keyword borgCfgAttribute remote_ssd_fs_dir replicas reschedule_timeout_ms
syn keyword borgCfgAttribute reset_job_failure_counts revert_on_error
syn keyword borgCfgAttribute rpcacl run_as_root run_updater_on_borg
syn keyword borgCfgAttribute schedule_type seastar security_mode segment
syn keyword borgCfgAttribute server_type serving_port shard shard_step
syn keyword borgCfgAttribute shardinfo_dir shardinfo_ports shared_cpu
syn keyword borgCfgAttribute shared_disk shared_milligcu shared_min_disks
syn keyword borgCfgAttribute shared_network_rate shared_num_disks shared_ram
syn keyword borgCfgAttribute shared_raw_ssds
syn keyword borgCfgAttribute size start_quorum static_port static_ports
syn keyword borgCfgAttribute status_port static_resources stop_time strategy
syn keyword borgCfgAttribute strict_confirmation_threshold swinetrek
syn keyword borgCfgAttribute task_rescheduling_policy task_update_order
syn keyword borgCfgAttribute timeout_seconds timeout_secs tos type
syn keyword borgCfgAttribute unencrypted_local_fs_dirs
syn keyword borgCfgAttribute unsafe_attr_limits_update use_dns use_fileutil
syn keyword borgCfgAttribute user visibility watch_duration_ms watch_interval_ms
syn keyword borgCfgAttribute watch_task_duration_ms watch_task_interval_ms

" Attributes within cdd object.
syn keyword borgCfgAttribute compiler allow_gzip message proto gcl_model
syn keyword borgCfgAttribute gclobjtype gclobject manual bns_name
syn keyword borgCfgAttribute max_versions_on_canaries task_count canary_tag
syn keyword borgCfgAttribute selector bns_to_validate_varz
syn keyword borgCfgAttribute notify_level disable_canaries_on_turnup
syn keyword borgCfgAttribute strict_proto_conversion

" Borg's builtin functions. Keep this in sync with:
" grep -h ConfFunction::Register google3/borg/configs/*.cc
"   | cut -d\" -f 2 | sort | fmt -50 | sed 's/^/syn keyword borgBuiltin /'
" and also update gcl.vim
syn keyword borgBuiltin borg_dns_name borg_username canonical_bcl
syn keyword borgBuiltin chroot_pb_str debugger_command format_backends
syn keyword borgBuiltin format_shardinfo get_binary get_job_bns_prefix
syn keyword borgBuiltin get_mpm_data get_mpm_labels get_mpm_version
syn keyword borgBuiltin get_mpm_versionmap get_num_tasks get_task_name
syn keyword borgBuiltin mkargs mkenv mk_extra_argv mkpath_exports
syn keyword borgBuiltin objectname package_dir packagename
syn keyword borgBuiltin real_username reduce_with_replicas shellescape
syn keyword borgBuiltin use_minicluster username

if version >= 508 || !exists("did_borg_syn_inits")
  if version <= 508
    let did_borg_syn_inits = 1
    command -nargs=+ HiLink hi link <args>
  else
    command -nargs=+ HiLink hi def link <args>
  endif

  " The default methods for highlighting.  Can be overridden later
  HiLink borgKeyword            gclKeyword
  HiLink borgCfgSection         gclSection
  HiLink borgCfgAttribute       gclAttribute
  HiLink borgBuiltin            gclBuiltin

  delcommand HiLink
endif

let b:current_syntax = "borg"

" vim: ts=8
