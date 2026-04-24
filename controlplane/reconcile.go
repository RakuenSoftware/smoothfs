package controlplane

import (
	"context"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type subtreeDir struct {
	rel   string
	depth int
}

func ReconcilePoolSubtrees(pool *Pool) error {
	if pool == nil || len(pool.Tiers) < 2 {
		return nil
	}

	for slowIdx := len(pool.Tiers) - 1; slowIdx > 0; slowIdx-- {
		slowTier := pool.Tiers[slowIdx]
		var dirs []subtreeDir

		err := filepath.WalkDir(slowTier.LowerDir, func(path string, d fs.DirEntry, err error) error {
			var rel string

			if err != nil || !d.IsDir() {
				return nil
			}
			if path == slowTier.LowerDir {
				return nil
			}
			if d.Name() == ".smoothfs" {
				return filepath.SkipDir
			}
			rel, err = filepath.Rel(slowTier.LowerDir, path)
			if err != nil || rel == "." {
				return nil
			}
			dirs = append(dirs, subtreeDir{
				rel:   rel,
				depth: strings.Count(rel, string(os.PathSeparator)),
			})
			return nil
		})
		if err != nil {
			return err
		}

		sort.Slice(dirs, func(i, j int) bool {
			if dirs[i].depth == dirs[j].depth {
				return dirs[i].rel > dirs[j].rel
			}
			return dirs[i].depth > dirs[j].depth
		})

		for _, dir := range dirs {
			slowPath := filepath.Join(slowTier.LowerDir, dir.rel)
			empty, err := isEmptyDir(slowPath)
			if err != nil || !empty {
				continue
			}
			if fasterTierHasDir(pool.Tiers[:slowIdx], dir.rel) {
				_ = os.Remove(slowPath)
			}
		}
	}

	return nil
}

func fasterTierHasDir(faster []TierInfo, rel string) bool {
	for _, tier := range faster {
		info, err := os.Stat(filepath.Join(tier.LowerDir, rel))
		if err == nil && info.IsDir() {
			return true
		}
	}
	return false
}

func isEmptyDir(path string) (bool, error) {
	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	defer f.Close()

	ents, err := f.ReadDir(1)
	if err != nil {
		if err == os.ErrNotExist {
			return false, nil
		}
		if err == io.EOF {
			return true, nil
		}
		return false, err
	}
	return len(ents) == 0, nil
}

func (s *Service) runSubtreeReconcile(ctx context.Context) {
	interval := time.Duration(s.planner.cfg.IntervalSec) * time.Second
	if interval <= 0 {
		interval = 15 * time.Minute
	}
	t := time.NewTicker(interval)
	defer t.Stop()

	reconcileAll := func() {
		s.mu.Lock()
		pools := make([]*Pool, 0, len(s.pools))
		for _, pool := range s.pools {
			pools = append(pools, pool)
		}
		s.mu.Unlock()

		for _, pool := range pools {
			if ctx.Err() != nil {
				return
			}
			_ = ReconcilePoolSubtrees(pool)
		}
	}

	reconcileAll()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			reconcileAll()
		}
	}
}
