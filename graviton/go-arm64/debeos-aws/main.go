// debeos-aws -- a minimal, native AWS client for DeBeOS on Haiku arm64.
//
// It exists to retire the ephemeral Linux publish peer (DeBeOS #120): the repo
// publish path needs bulk S3 (mirror ~1900 objects down, rebuild the index,
// mirror up with --delete) plus a CloudFront invalidation -- operations the
// single-file native S3 helper in debeos-ssm-agent cannot do. Full aws-cli is
// Python + C extensions; this covers the exact verbs the publish flow calls,
// in one CGO-free Go binary that cross-builds for GOOS=haiku GOARCH=arm64 with
// the korli-go toolchain and links pure-Go TLS (proven on Graviton, see
// graviton/docs/go-arm64-bringup-scope.md M2).
//
// Verbs (a deliberate subset of `aws s3` / `aws cloudfront`):
//
//	debeos-aws s3 ls   [s3://bucket[/prefix]]
//	debeos-aws s3 cp   <src> <dst>           # local<->s3 or s3<->s3, one object
//	debeos-aws s3 sync <src> <dst> [--delete] [--exclude GLOB] [--include GLOB]
//	debeos-aws s3 rm   <s3://bucket/key>
//	debeos-aws cloudfront create-invalidation --distribution-id ID --paths /a /b
//
// Credentials come from the default AWS chain (env, shared config, and -- the
// point for a headless Graviton box -- the EC2 instance-role provider over
// IMDS). Region: --region, AWS_REGION/AWS_DEFAULT_REGION, or IMDS placement.
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/feature/s3/manager"
	"github.com/aws/aws-sdk-go-v2/service/cloudfront"
	cftypes "github.com/aws/aws-sdk-go-v2/service/cloudfront/types"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/aws-sdk-go-v2/service/s3/types"
)

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	ctx := context.Background()
	var err error
	switch os.Args[1] {
	case "s3":
		err = runS3(ctx, os.Args[2:])
	case "cloudfront":
		err = runCloudFront(ctx, os.Args[2:])
	case "-h", "--help", "help":
		usage()
		return
	case "version":
		fmt.Println("debeos-aws " + version)
		return
	default:
		fmt.Fprintf(os.Stderr, "debeos-aws: unknown command %q\n", os.Args[1])
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "debeos-aws: "+err.Error())
		os.Exit(1)
	}
}

const version = "0.1.0"

func usage() {
	fmt.Fprint(os.Stderr, `usage:
  debeos-aws s3 ls   [s3://bucket[/prefix]]
  debeos-aws s3 cp   <src> <dst>           (local<->s3 or s3<->s3)
  debeos-aws s3 sync <src> <dst> [--delete] [--exclude GLOB] [--include GLOB]
  debeos-aws s3 rm   <s3://bucket/key>
  debeos-aws cloudfront create-invalidation --distribution-id ID --paths /a /b ...
  debeos-aws version

Credentials: default AWS chain (env, shared config, EC2 instance role via IMDS).
Region: --region flag, AWS_REGION/AWS_DEFAULT_REGION, or IMDS.
`)
}

// ---- shared config / clients -------------------------------------------------

// region is an optional global override parsed off any subcommand's args.
func loadConfig(ctx context.Context, region string) (aws.Config, error) {
	var opts []func(*config.LoadOptions) error
	if region != "" {
		opts = append(opts, config.WithRegion(region))
	}
	cfg, err := config.LoadDefaultConfig(ctx, opts...)
	if err != nil {
		return cfg, fmt.Errorf("load AWS config: %w", err)
	}
	if cfg.Region == "" {
		// IMDS did not yield a region and none was configured; default to the
		// project's home region rather than failing with an opaque SDK error.
		cfg.Region = "us-west-2"
	}
	return cfg, nil
}

// popRegion strips a "--region R" (or "--region=R") pair from args, returning
// the value and the remaining args. Keeps every verb region-aware uniformly.
func popRegion(args []string) (string, []string) {
	region := os.Getenv("AWS_REGION")
	if region == "" {
		region = os.Getenv("AWS_DEFAULT_REGION")
	}
	out := args[:0:0]
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch {
		case a == "--region" && i+1 < len(args):
			region = args[i+1]
			i++
		case strings.HasPrefix(a, "--region="):
			region = a[len("--region="):]
		default:
			out = append(out, a)
		}
	}
	return region, out
}

// ---- S3 URI parsing ----------------------------------------------------------

type s3ref struct {
	bucket string
	key    string // may be empty (bucket root) or end in "/" (prefix)
}

func isS3(s string) bool { return strings.HasPrefix(s, "s3://") }

func parseS3(s string) (s3ref, error) {
	if !isS3(s) {
		return s3ref{}, fmt.Errorf("not an s3 uri: %q", s)
	}
	rest := strings.TrimPrefix(s, "s3://")
	b, k, _ := strings.Cut(rest, "/")
	if b == "" {
		return s3ref{}, fmt.Errorf("empty bucket in %q", s)
	}
	return s3ref{bucket: b, key: k}, nil
}

// ---- s3 dispatch -------------------------------------------------------------

func runS3(ctx context.Context, args []string) error {
	if len(args) < 1 {
		return errors.New("s3: need a subcommand (ls|cp|sync|rm)")
	}
	sub := args[0]
	region, rest := popRegion(args[1:])
	cfg, err := loadConfig(ctx, region)
	if err != nil {
		return err
	}
	cl := s3.NewFromConfig(cfg)
	switch sub {
	case "ls":
		return s3ls(ctx, cl, rest)
	case "cp":
		return s3cp(ctx, cfg, cl, rest)
	case "sync":
		return s3sync(ctx, cfg, cl, rest)
	case "rm":
		return s3rm(ctx, cl, rest)
	default:
		return fmt.Errorf("s3: unknown subcommand %q", sub)
	}
}

// s3 ls -----------------------------------------------------------------------

func s3ls(ctx context.Context, cl *s3.Client, args []string) error {
	if len(args) == 0 {
		// list buckets
		out, err := cl.ListBuckets(ctx, &s3.ListBucketsInput{})
		if err != nil {
			return err
		}
		for _, b := range out.Buckets {
			fmt.Printf("%s %s\n", b.CreationDate.Format("2006-01-02 15:04:05"), aws.ToString(b.Name))
		}
		return nil
	}
	ref, err := parseS3(args[0])
	if err != nil {
		return err
	}
	prefix := ref.key
	p := s3.NewListObjectsV2Paginator(cl, &s3.ListObjectsV2Input{
		Bucket:    &ref.bucket,
		Prefix:    &prefix,
		Delimiter: aws.String("/"),
	})
	for p.HasMorePages() {
		page, err := p.NextPage(ctx)
		if err != nil {
			return err
		}
		for _, cp := range page.CommonPrefixes {
			fmt.Printf("%28s %s\n", "PRE", aws.ToString(cp.Prefix))
		}
		for _, o := range page.Contents {
			fmt.Printf("%s %10d %s\n",
				aws.ToTime(o.LastModified).Format("2006-01-02 15:04:05"),
				aws.ToInt64(o.Size), aws.ToString(o.Key))
		}
	}
	return nil
}

// s3 cp -----------------------------------------------------------------------

func s3cp(ctx context.Context, cfg aws.Config, cl *s3.Client, args []string) error {
	if len(args) != 2 {
		return errors.New("s3 cp: need <src> <dst>")
	}
	src, dst := args[0], args[1]
	switch {
	case isS3(src) && isS3(dst):
		s, _ := parseS3(src)
		d, _ := parseS3(dst)
		_, err := cl.CopyObject(ctx, &s3.CopyObjectInput{
			Bucket:     &d.bucket,
			Key:        &d.key,
			CopySource: aws.String(s.bucket + "/" + s.key),
		})
		return err
	case isS3(src):
		s, _ := parseS3(src)
		local := dst
		if strings.HasSuffix(dst, "/") || isDir(dst) {
			local = filepath.Join(dst, path.Base(s.key))
		}
		return downloadOne(ctx, cfg, s, local)
	case isS3(dst):
		d, _ := parseS3(dst)
		if d.key == "" || strings.HasSuffix(d.key, "/") {
			d.key = strings.TrimSuffix(d.key, "/") + "/" + filepath.Base(src)
			d.key = strings.TrimPrefix(d.key, "/")
		}
		return uploadOne(ctx, cfg, src, d)
	default:
		return errors.New("s3 cp: at least one of src/dst must be an s3:// uri")
	}
}

func downloadOne(ctx context.Context, cfg aws.Config, s s3ref, local string) error {
	if dir := filepath.Dir(local); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	f, err := os.Create(local)
	if err != nil {
		return err
	}
	defer f.Close()
	d := manager.NewDownloader(s3.NewFromConfig(cfg))
	n, err := d.Download(ctx, f, &s3.GetObjectInput{Bucket: &s.bucket, Key: &s.key})
	if err != nil {
		return err
	}
	fmt.Printf("download: s3://%s/%s -> %s (%d bytes)\n", s.bucket, s.key, local, n)
	return nil
}

func uploadOne(ctx context.Context, cfg aws.Config, local string, d s3ref) error {
	f, err := os.Open(local)
	if err != nil {
		return err
	}
	defer f.Close()
	u := manager.NewUploader(s3.NewFromConfig(cfg))
	_, err = u.Upload(ctx, &s3.PutObjectInput{Bucket: &d.bucket, Key: &d.key, Body: f})
	if err != nil {
		return err
	}
	fmt.Printf("upload: %s -> s3://%s/%s\n", local, d.bucket, d.key)
	return nil
}

// s3 rm -----------------------------------------------------------------------

func s3rm(ctx context.Context, cl *s3.Client, args []string) error {
	if len(args) != 1 || !isS3(args[0]) {
		return errors.New("s3 rm: need one s3://bucket/key")
	}
	r, _ := parseS3(args[0])
	_, err := cl.DeleteObject(ctx, &s3.DeleteObjectInput{Bucket: &r.bucket, Key: &r.key})
	if err == nil {
		fmt.Printf("delete: s3://%s/%s\n", r.bucket, r.key)
	}
	return err
}

// s3 sync ---------------------------------------------------------------------

type syncOpts struct {
	del     bool
	filters []filterRule // ordered; last match wins (awscli semantics)
}

type filterRule struct {
	include bool
	glob    string
}

// matches applies the ordered include/exclude rules to a relative key. Default
// (no rule matches) is include, matching `aws s3 sync`.
func (o syncOpts) matches(rel string) bool {
	inc := true
	for _, r := range o.filters {
		if ok, _ := path.Match(r.glob, rel); ok {
			inc = r.include
		}
		// awscli also matches the basename for a bare pattern; approximate by
		// also testing the last path element.
		if ok, _ := path.Match(r.glob, path.Base(rel)); ok {
			inc = r.include
		}
	}
	return inc
}

func parseSyncArgs(args []string) (src, dst string, o syncOpts, err error) {
	var pos []string
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--delete":
			o.del = true
		case "--exclude":
			if i+1 >= len(args) {
				return "", "", o, errors.New("--exclude needs a value")
			}
			o.filters = append(o.filters, filterRule{include: false, glob: args[i+1]})
			i++
		case "--include":
			if i+1 >= len(args) {
				return "", "", o, errors.New("--include needs a value")
			}
			o.filters = append(o.filters, filterRule{include: true, glob: args[i+1]})
			i++
		default:
			pos = append(pos, args[i])
		}
	}
	if len(pos) != 2 {
		return "", "", o, errors.New("s3 sync: need <src> <dst>")
	}
	return pos[0], pos[1], o, nil
}

type objMeta struct {
	size    int64
	modTime time.Time
}

func s3sync(ctx context.Context, cfg aws.Config, cl *s3.Client, args []string) error {
	src, dst, o, err := parseSyncArgs(args)
	if err != nil {
		return err
	}
	switch {
	case isS3(src) && !isS3(dst):
		return syncDown(ctx, cfg, cl, src, dst, o)
	case !isS3(src) && isS3(dst):
		return syncUp(ctx, cfg, cl, src, dst, o)
	default:
		return errors.New("s3 sync: exactly one of src/dst must be s3:// (local<->s3 only)")
	}
}

func listS3(ctx context.Context, cl *s3.Client, r s3ref) (map[string]objMeta, error) {
	prefix := strings.TrimSuffix(r.key, "/")
	if prefix != "" {
		prefix += "/"
	}
	out := map[string]objMeta{}
	p := s3.NewListObjectsV2Paginator(cl, &s3.ListObjectsV2Input{Bucket: &r.bucket, Prefix: &prefix})
	for p.HasMorePages() {
		page, err := p.NextPage(ctx)
		if err != nil {
			return nil, err
		}
		for _, obj := range page.Contents {
			key := aws.ToString(obj.Key)
			rel := strings.TrimPrefix(key, prefix)
			if rel == "" || strings.HasSuffix(rel, "/") {
				continue
			}
			out[rel] = objMeta{size: aws.ToInt64(obj.Size), modTime: aws.ToTime(obj.LastModified)}
		}
	}
	return out, nil
}

func listLocal(root string) (map[string]objMeta, error) {
	out := map[string]objMeta{}
	err := filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if fi.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return err
		}
		out[filepath.ToSlash(rel)] = objMeta{size: fi.Size(), modTime: fi.ModTime()}
		return nil
	})
	if os.IsNotExist(err) {
		return out, nil
	}
	return out, err
}

// needCopy mirrors awscli's default comparator: copy if the dest is missing,
// the size differs, or the source is newer than the dest.
func needCopy(src, dst objMeta, ok bool) bool {
	if !ok {
		return true
	}
	if src.size != dst.size {
		return true
	}
	return src.modTime.After(dst.modTime)
}

func syncDown(ctx context.Context, cfg aws.Config, cl *s3.Client, src, dstDir string, o syncOpts) error {
	r, err := parseS3(src)
	if err != nil {
		return err
	}
	srcObjs, err := listS3(ctx, cl, r)
	if err != nil {
		return err
	}
	dstObjs, _ := listLocal(dstDir)
	prefix := strings.TrimSuffix(r.key, "/")
	if prefix != "" {
		prefix += "/"
	}
	dl := manager.NewDownloader(cl)
	var copied, deleted int
	for _, rel := range sortedKeys(srcObjs) {
		if !o.matches(rel) {
			continue
		}
		if !needCopy(srcObjs[rel], dstObjs[rel], func() bool { _, ok := dstObjs[rel]; return ok }()) {
			continue
		}
		local := filepath.Join(dstDir, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(local), 0o755); err != nil {
			return err
		}
		f, err := os.Create(local)
		if err != nil {
			return err
		}
		key := prefix + rel
		if _, err := dl.Download(ctx, f, &s3.GetObjectInput{Bucket: &r.bucket, Key: &key}); err != nil {
			f.Close()
			return err
		}
		f.Close()
		copied++
	}
	if o.del {
		for rel := range dstObjs {
			if _, ok := srcObjs[rel]; !ok && o.matches(rel) {
				if err := os.Remove(filepath.Join(dstDir, filepath.FromSlash(rel))); err != nil {
					return err
				}
				deleted++
			}
		}
	}
	fmt.Printf("sync down: %d copied, %d deleted\n", copied, deleted)
	return nil
}

func syncUp(ctx context.Context, cfg aws.Config, cl *s3.Client, srcDir, dst string, o syncOpts) error {
	r, err := parseS3(dst)
	if err != nil {
		return err
	}
	srcObjs, err := listLocal(srcDir)
	if err != nil {
		return err
	}
	dstObjs, err := listS3(ctx, cl, r)
	if err != nil {
		return err
	}
	prefix := strings.TrimSuffix(r.key, "/")
	if prefix != "" {
		prefix += "/"
	}
	up := manager.NewUploader(cl)
	var copied, deleted int
	for _, rel := range sortedKeys(srcObjs) {
		if !o.matches(rel) {
			continue
		}
		if !needCopy(srcObjs[rel], dstObjs[rel], func() bool { _, ok := dstObjs[rel]; return ok }()) {
			continue
		}
		f, err := os.Open(filepath.Join(srcDir, filepath.FromSlash(rel)))
		if err != nil {
			return err
		}
		key := prefix + rel
		if _, err := up.Upload(ctx, &s3.PutObjectInput{Bucket: &r.bucket, Key: &key, Body: f}); err != nil {
			f.Close()
			return err
		}
		f.Close()
		copied++
	}
	if o.del {
		var toDel []types.ObjectIdentifier
		for rel := range dstObjs {
			if _, ok := srcObjs[rel]; !ok && o.matches(rel) {
				k := prefix + rel
				toDel = append(toDel, types.ObjectIdentifier{Key: aws.String(k)})
			}
		}
		for i := 0; i < len(toDel); i += 1000 {
			end := i + 1000
			if end > len(toDel) {
				end = len(toDel)
			}
			out, err := cl.DeleteObjects(ctx, &s3.DeleteObjectsInput{
				Bucket: &r.bucket,
				Delete: &types.Delete{Objects: toDel[i:end], Quiet: aws.Bool(true)},
			})
			if err != nil {
				return err
			}
			// Quiet mode suppresses successes but still returns per-key Errors;
			// a batch call can succeed at the HTTP layer while every delete is
			// denied. Surface that rather than reporting a false "deleted".
			if len(out.Errors) > 0 {
				e := out.Errors[0]
				return fmt.Errorf("delete %s: %s: %s (and %d more)",
					aws.ToString(e.Key), aws.ToString(e.Code),
					aws.ToString(e.Message), len(out.Errors)-1)
			}
			deleted += end - i
		}
	}
	fmt.Printf("sync up: %d copied, %d deleted\n", copied, deleted)
	return nil
}

// ---- cloudfront --------------------------------------------------------------

func runCloudFront(ctx context.Context, args []string) error {
	if len(args) < 1 || args[0] != "create-invalidation" {
		return errors.New("cloudfront: only 'create-invalidation' is supported")
	}
	region, rest := popRegion(args[1:])
	var distID string
	var paths []string
	for i := 0; i < len(rest); i++ {
		switch {
		case rest[i] == "--distribution-id" && i+1 < len(rest):
			distID = rest[i+1]
			i++
		case strings.HasPrefix(rest[i], "--distribution-id="):
			distID = rest[i][len("--distribution-id="):]
		case rest[i] == "--paths":
			for i+1 < len(rest) && !strings.HasPrefix(rest[i+1], "--") {
				paths = append(paths, rest[i+1])
				i++
			}
		}
	}
	if distID == "" {
		return errors.New("cloudfront create-invalidation: --distribution-id required")
	}
	if len(paths) == 0 {
		paths = []string{"/*"}
	}
	cfg, err := loadConfig(ctx, region)
	if err != nil {
		return err
	}
	cl := cloudfront.NewFromConfig(cfg)
	ref := fmt.Sprintf("debeos-aws-%d", time.Now().UnixNano())
	out, err := cl.CreateInvalidation(ctx, &cloudfront.CreateInvalidationInput{
		DistributionId: &distID,
		InvalidationBatch: &cftypes.InvalidationBatch{
			CallerReference: &ref,
			Paths: &cftypes.Paths{
				Quantity: aws.Int32(int32(len(paths))),
				Items:    paths,
			},
		},
	})
	if err != nil {
		return err
	}
	fmt.Printf("invalidation: %s status=%s\n", aws.ToString(out.Invalidation.Id), aws.ToString(out.Invalidation.Status))
	return nil
}

// ---- helpers -----------------------------------------------------------------

func isDir(p string) bool {
	fi, err := os.Stat(p)
	return err == nil && fi.IsDir()
}

func sortedKeys(m map[string]objMeta) []string {
	ks := make([]string, 0, len(m))
	for k := range m {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	return ks
}
