if exists('b:citc_root')
  let &l:path='.,'.fnamemodify(b:citc_root, ':h').','.b:citc_root.'/third_party/py'
endif

