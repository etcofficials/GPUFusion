# Headless check of GPU Fusion for Godot:
#   Godot --headless --path sdk/godot/demo --script res://check.gd -- [image path] [radius] [runs]
# Blurs a photograph with the extension on the primary GPU alone and on every GPU, checks every result
# against the CPU reference bit for bit, and prints one JSON line with the times and the division of
# rows. Exit code 0 = every result correct.
extends SceneTree

func median(values: Array) -> float:
	var s := values.duplicate()
	s.sort()
	return s[s.size() / 2]

func measure(fx, img: Image, radius: int, runs: int, reference: PackedByteArray) -> Dictionary:
	var times := []
	var correct := true
	var both := 0
	var rows := {}
	# Warm-up: wakes the GPUs from their idle clocks; the scheduler measures the primary alone first.
	for i in range(4):
		fx.blur(img, radius)
	for i in range(runs):
		var t := Time.get_ticks_usec()
		var out: Image = fx.blur(img, radius)
		times.append((Time.get_ticks_usec() - t) / 1000.0)
		correct = correct and out != null and out.get_data() == reference
		var r: Dictionary = fx.last_report()
		both += 1 if r["both_gpus"] else 0
		for g in r["gpus"]:
			rows[g["name"]] = rows.get(g["name"], 0) + g["rows"]
	return {"median_ms": median(times), "min_ms": times.min(), "max_ms": times.max(), "runs": runs, "correct": correct,
		"runs_on_both_gpus": both, "rows_per_gpu": rows, "last_plan": fx.last_report()["plan"]}

func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var path: String = args[0] if args.size() > 0 else "C:/Windows/Web/Wallpaper/Theme1/img1.jpg"
	var radius: int = int(args[1]) if args.size() > 1 else 6
	var runs: int = int(args[2]) if args.size() > 2 else 12
	var img := Image.load_from_file(path)
	if img == null:
		print(JSON.stringify({"error": "cannot load " + path}))
		quit(1)
		return
	img.convert(Image.FORMAT_RGBA8)
	var fx = ClassDB.instantiate("GPUFusionCompute")
	if fx == null:
		print(JSON.stringify({"error": "the GPUFusionCompute class is missing: the extension did not load"}))
		quit(1)
		return
	var reference: PackedByteArray = fx.blur_reference(img, radius).get_data()
	fx.configure(0, 1)
	var gpus_one: PackedStringArray = fx.adapters()
	var one := measure(fx, img, radius, runs, reference)
	fx.configure(0, 8)
	var gpus_all: PackedStringArray = fx.adapters()
	var all := measure(fx, img, radius, runs, reference)
	var result := {"godot": Engine.get_version_info()["string"], "image": path, "width": img.get_width(), "height": img.get_height(),
		"radius": radius, "primary_alone": {"gpus": gpus_one, "result": one}, "all_gpus": {"gpus": gpus_all, "result": all},
		"speedup": one["median_ms"] / all["median_ms"], "learned_state": fx.learned_state()}
	print(JSON.stringify(result))
	quit(0 if one["correct"] and all["correct"] else 1)
