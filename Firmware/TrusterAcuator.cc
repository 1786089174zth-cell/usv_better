#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct MapperConfig {
	int pwm_period_ns = 20000000;
	int duty_neutral_ns = 1500000;
	int duty_span_ns = 500000;

	float max_power_percent = 70.0f;
	float left_gain = 1.0f;
	float right_gain = 1.0f;
	float left_trim = 0.0f;
	float right_trim = 0.0f;
};

struct ThrusterInput {
	float left_percent = 0.0f;
	float right_percent = 0.0f;
};

struct ThrusterOutput {
	float left_percent_final = 0.0f;
	float right_percent_final = 0.0f;
	int left_duty_ns = 0;
	int right_duty_ns = 0;
	float applied_limit_percent = 0.0f;
	int status_code = 0;
};

class ThrusterMapper {
public:
	explicit ThrusterMapper(MapperConfig cfg) : cfg_(cfg) {}

	ThrusterOutput apply(const ThrusterInput& in) const {
		ThrusterOutput out;

		float left = in.left_percent;
		float right = in.right_percent;

		const float limit = std::max(0.0f, std::min(100.0f, cfg_.max_power_percent));
		left = clamp(left, -limit, limit);
		right = clamp(right, -limit, limit);

		left = left * cfg_.left_gain + cfg_.left_trim;
		right = right * cfg_.right_gain + cfg_.right_trim;

		left = clamp(left, 0.0f, 100.0f);
		right = clamp(right, 0.0f, 100.0f);

		out.left_percent_final = left;
		out.right_percent_final = right;
		out.left_duty_ns = percentToDutyNs(left);
		out.right_duty_ns = percentToDutyNs(right);
		out.applied_limit_percent = limit;
		out.status_code = 0;
		return out;
	}

private:
	static float clamp(float v, float lo, float hi) {
		return std::max(lo, std::min(hi, v));
	}

	int percentToDutyNs(float percent) const {
		const float ratio = percent / 100.0f;
		const float duty = static_cast<float>(cfg_.duty_neutral_ns) +
						   ratio * static_cast<float>(cfg_.duty_span_ns);
		const int min_duty = cfg_.duty_neutral_ns - cfg_.duty_span_ns;
		const int max_duty = cfg_.duty_neutral_ns + cfg_.duty_span_ns;
		return static_cast<int>(std::round(clamp(duty, static_cast<float>(min_duty),
												 static_cast<float>(max_duty))));
	}

	MapperConfig cfg_;
};

class SysfsPwmWriter {
public:
	SysfsPwmWriter(std::string left_duty_path, std::string right_duty_path, bool dry_run)
		: left_duty_path_(std::move(left_duty_path)),
		  right_duty_path_(std::move(right_duty_path)),
		  dry_run_(dry_run) {}

	bool writeDuty(int left_duty_ns, int right_duty_ns) const {
		if (dry_run_) {
			std::cout << "[DRY-RUN] write duty_ns: left=" << left_duty_ns
					  << ", right=" << right_duty_ns << std::endl;
			return true;
		}

		if (!writeInt(left_duty_path_, left_duty_ns)) {
			std::cerr << "ERR: failed writing left duty path" << std::endl;
			return false;
		}
		if (!writeInt(right_duty_path_, right_duty_ns)) {
			std::cerr << "ERR: failed writing right duty path" << std::endl;
			return false;
		}
		return true;
	}

private:
	static bool writeInt(const std::string& path, int value) {
		std::ofstream file(path);
		if (!file.is_open()) {
			std::cerr << "ERR: open failed: " << path << std::endl;
			return false;
		}
		file << value << '\n';
		return file.good();
	}

	std::string left_duty_path_;
	std::string right_duty_path_;
	bool dry_run_;
};
void printUsage() {
	std::cout << "Thruster executor demo\n"
			  << "Input format: <left_percent> <right_percent>\n"
			  << "Example: 30 -25\n"
			  << "Type q to quit.\n";
}

}  // namespace

int main() {
	MapperConfig cfg;
	cfg.max_power_percent = 70.0f;
	cfg.left_gain = 1.0f;
	cfg.right_gain = 0.98f;
	cfg.left_trim = 0.0f;
	cfg.right_trim = 0.0f;

	ThrusterMapper mapper(cfg);

	// Default to dry-run so this binary can run safely on non-Orangepi hosts.
	const bool dry_run = true;
	SysfsPwmWriter writer(
		"/sys/class/pwm/pwmchip0/pwm1/duty_cycle",
		"/sys/class/pwm/pwmchip0/pwm2/duty_cycle",
		dry_run);

	printUsage();
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line == "q" || line == "Q") {
			break;
		}

		std::istringstream iss(line);
		float left = 0.0f;
		float right = 0.0f;
		if (!(iss >> left >> right)) {
			std::cout << "WARN: invalid input, expect two numbers." << std::endl;
			continue;
		}

		ThrusterInput in;
		in.left_percent = left;
		in.right_percent = right;

		const ThrusterOutput out = mapper.apply(in);
		const bool ok = writer.writeDuty(out.left_duty_ns, out.right_duty_ns);
		std::cout << std::fixed << std::setprecision(2)
				  << "left=" << out.left_percent_final
				  << "%, right=" << out.right_percent_final
				  << "%, status=" << (ok ? 0 : 1) << std::endl;
	}

	// Fail-safe stop on exit.
	writer.writeDuty(0, 0);
	return 0;
}
