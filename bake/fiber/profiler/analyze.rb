# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

# @parameter input [Input] The input to process.
def analyze(input:)
	summary = {}
	
	input.each do |sample|
		duration = sample["duration"]
		calls = sample["calls"]
		
		if duration and calls
			calls.each do |call|
				location = "#{call["path"]}:#{call["line"]}"
				
				summary[location] ||= {duration: 0, calls: 0, class: call["class"], method: call["method"]}
				summary[location][:duration] += call["duration"]
				summary[location][:calls] += 1
			end
		end
	end
	
	summary.sort_by{|location, data| -data[:duration]}
end
