/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/audio/media_audio_edit.h"

#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"

namespace Media {
namespace Audio {
namespace {

constexpr auto kMaxVideoBytes = int64(256) * 1024 * 1024;
constexpr auto kMaxAudioBytes = int64(64) * 1024 * 1024;
constexpr auto kMaxAudioPackets = 100000;

bool HasEncryptionSideData(const AVPacketSideData *data, int count) {
	return av_packet_side_data_get(data, count, AV_PKT_DATA_ENCRYPTION_INFO)
		|| av_packet_side_data_get(
			data,
			count,
			AV_PKT_DATA_ENCRYPTION_INIT_INFO);
}

int64 SeekPosition(int64 offset, int whence, int64 current, int64 size) {
	whence &= ~AVSEEK_FORCE;
	if (whence == AVSEEK_SIZE) {
		return size;
	}
	const auto base = (whence == SEEK_SET) ? int64(0)
		: (whence == SEEK_CUR) ? current
		: (whence == SEEK_END) ? size
		: int64(-1);
	if (base < 0 || offset < -base || offset > size - base) {
		return AVERROR(EINVAL);
	}
	return base + offset;
}

int64_t SeekInput(void *opaque, int64_t offset, int whence) {
	const auto wrap = static_cast<FFmpeg::ReadBytesWrap*>(opaque);
	const auto result = SeekPosition(offset, whence, wrap->offset, wrap->size);
	if (result >= 0 && (whence & ~AVSEEK_FORCE) != AVSEEK_SIZE) {
		wrap->offset = result;
	}
	return result;
}

struct BoundedOutput {
	FFmpeg::WriteBytesWrap wrap;

#if DA_FFMPEG_CONST_WRITE_CALLBACK
	static int Write(void *opaque, const uint8_t *buffer, int size) {
#else
	static int Write(void *opaque, uint8_t *buffer, int size) {
#endif
		const auto self = static_cast<BoundedOutput*>(opaque);
		if (size < 0 || size > kMaxAudioBytes - self->wrap.offset) {
			return AVERROR(ENOSPC);
		}
		return FFmpeg::WriteBytesWrap::Write(&self->wrap, buffer, size);
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		const auto self = static_cast<BoundedOutput*>(opaque);
		const auto result = SeekPosition(
			offset,
			whence,
			self->wrap.offset,
			self->wrap.content.size());
		if (result >= 0 && (whence & ~AVSEEK_FORCE) != AVSEEK_SIZE) {
			self->wrap.offset = result;
		}
		return result;
	}
};

}

QByteArray ExtractVideoAudio(const QByteArray &video) {
	using namespace FFmpeg;

	if (video.isEmpty() || video.size() > kMaxVideoBytes) {
		return {};
	}
	auto inputWrap = ReadBytesWrap{
		.size = video.size(),
		.data = reinterpret_cast<const uchar*>(video.constData()),
	};
	auto input = MakeFormatPointer(
		&inputWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&SeekInput);
	if (!input
		|| input->iformat != av_find_input_format("mov")
		|| AvErrorWrap(avformat_find_stream_info(input.get(), nullptr))) {
		return {};
	}
	const auto streamId = av_find_best_stream(
		input.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		return {};
	}
	const auto inStream = input->streams[streamId];
	if (inStream->codecpar->codec_id != AV_CODEC_ID_AAC
		|| !inStream->codecpar->extradata
		|| inStream->codecpar->extradata_size <= 0
		|| HasEncryptionSideData(
			inStream->codecpar->coded_side_data,
			inStream->codecpar->nb_coded_side_data)
		|| inStream->time_base.num <= 0
		|| inStream->time_base.den <= 0) {
		return {};
	}
	for (auto i = 0U; i != input->nb_streams; ++i) {
		if (i != unsigned(streamId)) {
			input->streams[i]->discard = AVDISCARD_ALL;
		}
	}

	auto outputWrap = BoundedOutput();
	auto output = MakeWriteFormatPointer(
		&outputWrap,
		nullptr,
		&BoundedOutput::Write,
		&BoundedOutput::Seek,
		"mp4"_q);
	if (!output) {
		return {};
	}
	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream || AvErrorWrap(avcodec_parameters_copy(
			outStream->codecpar,
			inStream->codecpar))) {
		return {};
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = inStream->time_base;
	if (AvErrorWrap(avformat_write_header(output.get(), nullptr))
		|| output->pb->error < 0
		|| outStream->time_base.num <= 0
		|| outStream->time_base.den <= 0) {
		return {};
	}

	auto packet = AVPacket();
	av_init_packet(&packet);
	auto processed = 0;
	auto copied = 0;
	while (true) {
		const auto error = AvErrorWrap(av_read_frame(input.get(), &packet));
		if (error.code() == AVERROR_EOF) {
			if (input->pb->error < 0 && input->pb->error != AVERROR_EOF) {
				return {};
			}
			break;
		} else if (error) {
			return {};
		}
		const auto guard = gsl::finally([&] {
			av_packet_unref(&packet);
		});
		if (packet.stream_index != streamId) {
			continue;
		}
		if (++processed > kMaxAudioPackets
			|| HasEncryptionSideData(packet.side_data, packet.side_data_elems)) {
			return {};
		}
		if (packet.flags & AV_PKT_FLAG_DISCARD) {
			if (copied) {
				return {};
			}
			continue;
		}
		if (packet.size <= 0
			|| (packet.flags & AV_PKT_FLAG_CORRUPT)
			|| packet.pts == AV_NOPTS_VALUE
			|| packet.dts == AV_NOPTS_VALUE
			|| packet.duration < 0) {
			return {};
		}
		++copied;
		av_packet_rescale_ts(&packet, inStream->time_base, outStream->time_base);
		packet.stream_index = outStream->index;
		packet.pos = -1;
		if (AvErrorWrap(av_interleaved_write_frame(output.get(), &packet))
			|| output->pb->error < 0) {
			return {};
		}
	}
	if (!copied || AvErrorWrap(av_write_trailer(output.get()))) {
		return {};
	}
	avio_flush(output->pb);
	if (output->pb->error < 0) {
		return {};
	}
	return std::move(outputWrap.wrap.content);
}

}

[[nodiscard]] AudioEditResult TrimAudioToRange(
		const QByteArray &content,
		crl::time from,
		crl::time till) {
	using namespace FFmpeg;

	if (content.isEmpty() || (from < 0) || (till <= from)) {
		return {};
	}

	auto inputWrap = ReadBytesWrap{
		.size = content.size(),
		.data = reinterpret_cast<const uchar*>(content.constData()),
	};
	auto input = MakeFormatPointer(
		&inputWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!input) {
		return {};
	}

	auto error = AvErrorWrap(avformat_find_stream_info(input.get(), nullptr));
	if (error) {
		LogError(u"avformat_find_stream_info"_q, error);
		return {};
	}

	const auto streamId = av_find_best_stream(
		input.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamId < 0) {
		LogError(u"av_find_best_stream"_q, AvErrorWrap(streamId));
		return {};
	}

	const auto inStream = input->streams[streamId];
	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return {};
	}

	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		LogError(u"avformat_new_stream"_q);
		return {};
	}

	error = AvErrorWrap(avcodec_parameters_copy(
		outStream->codecpar,
		inStream->codecpar));
	if (error) {
		LogError(u"avcodec_parameters_copy"_q, error);
		return {};
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = inStream->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		LogError(u"avformat_write_header"_q, error);
		return {};
	}

	const auto fromPts = TimeToPts(from, inStream->time_base);
	const auto tillPts = TimeToPts(till, inStream->time_base);
	auto firstPts = int64(AV_NOPTS_VALUE);
	auto firstDts = int64(AV_NOPTS_VALUE);
	auto lastPts = std::numeric_limits<int64>::min();
	auto lastDts = std::numeric_limits<int64>::min();
	auto durationPts = int64(0);
	auto copied = 0;

	auto packetWrap = Packet();
	auto &packet = packetWrap.fields();
	while (true) {
		error = AvErrorWrap(av_read_frame(input.get(), &packet));
		if (error.code() == AVERROR_EOF) {
			break;
		} else if (error) {
			LogError(u"av_read_frame"_q, error);
			return {};
		}
		const auto guard = gsl::finally([&] {
			av_packet_unref(&packet);
		});
		if (packet.stream_index != streamId) {
			continue;
		}

		const auto packetStart = (packet.pts != AV_NOPTS_VALUE)
			? packet.pts
			: packet.dts;
		const auto packetDuration = std::max(int64(packet.duration), int64());
		const auto packetEnd = (packetStart != AV_NOPTS_VALUE)
			? (packetStart + packetDuration)
			: AV_NOPTS_VALUE;
		if ((packetStart != AV_NOPTS_VALUE) && (packetStart >= tillPts)) {
			break;
		}
		if ((packetEnd != AV_NOPTS_VALUE) && (packetEnd <= fromPts)) {
			continue;
		}

		if (packet.pts != AV_NOPTS_VALUE) {
			if (firstPts == AV_NOPTS_VALUE) {
				firstPts = packet.pts;
			}
			packet.pts -= firstPts;
			if (packet.pts < 0) {
				packet.pts = 0;
			}
			if (packet.pts <= lastPts) {
				packet.pts = lastPts + 1;
			}
			lastPts = packet.pts;
		}
		if (packet.dts != AV_NOPTS_VALUE) {
			if (firstDts == AV_NOPTS_VALUE) {
				firstDts = packet.dts;
			}
			packet.dts -= firstDts;
			if (packet.dts < 0) {
				packet.dts = 0;
			}
			if (packet.dts <= lastDts) {
				packet.dts = lastDts + 1;
			}
			lastDts = packet.dts;
		}

		const auto packetPosition = (packet.pts != AV_NOPTS_VALUE)
			? packet.pts
			: packet.dts;
		if (packetPosition != AV_NOPTS_VALUE) {
			durationPts = std::max(
				durationPts,
				packetPosition + std::max(int64(packet.duration), int64()));
		}

		packet.stream_index = outStream->index;
		error = AvErrorWrap(av_interleaved_write_frame(output.get(), &packet));
		if (error) {
			LogError(u"av_interleaved_write_frame"_q, error);
			return {};
		}
		++copied;
	}

	if (!copied) {
		return {};
	}

	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		LogError(u"av_write_trailer"_q, error);
		return {};
	}

	auto result = AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.waveform = audioCountWaveform(Core::FileLocation(), result.content);
	result.duration = durationPts
		? PtsToTimeCeil(durationPts, outStream->time_base)
		: (till - from);
	return result;
}

[[nodiscard]] AudioEditResult ConcatAudio(
		const QByteArray &first,
		const QByteArray &second) {
	using namespace FFmpeg;

	if (first.isEmpty() || second.isEmpty()) {
		return {};
	}

	auto firstWrap = ReadBytesWrap{
		.size = first.size(),
		.data = reinterpret_cast<const uchar*>(first.constData()),
	};
	auto firstInput = MakeFormatPointer(
		&firstWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!firstInput) {
		return {};
	}

	auto secondWrap = ReadBytesWrap{
		.size = second.size(),
		.data = reinterpret_cast<const uchar*>(second.constData()),
	};
	auto secondInput = MakeFormatPointer(
		&secondWrap,
		&ReadBytesWrap::Read,
		nullptr,
		&ReadBytesWrap::Seek);
	if (!secondInput) {
		return {};
	}

	const auto prepareStream = [](not_null<AVFormatContext*> input) {
		auto error = AvErrorWrap(avformat_find_stream_info(input, nullptr));
		if (error) {
			LogError(u"avformat_find_stream_info"_q, error);
			return static_cast<AVStream*>(nullptr);
		}
		const auto streamId = av_find_best_stream(
			input,
			AVMEDIA_TYPE_AUDIO,
			-1,
			-1,
			nullptr,
			0);
		if (streamId < 0) {
			LogError(u"av_find_best_stream"_q, AvErrorWrap(streamId));
			return static_cast<AVStream*>(nullptr);
		}
		return input->streams[streamId];
	};
	const auto firstStream = prepareStream(firstInput.get());
	if (!firstStream) {
		return {};
	}
	const auto secondStream = prepareStream(secondInput.get());
	if (!secondStream) {
		return {};
	}

	auto outputWrap = WriteBytesWrap();
	auto output = MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&WriteBytesWrap::Write,
		&WriteBytesWrap::Seek,
		"opus"_q);
	if (!output) {
		return {};
	}
	const auto outStream = avformat_new_stream(output.get(), nullptr);
	if (!outStream) {
		LogError(u"avformat_new_stream"_q);
		return {};
	}

	auto error = AvErrorWrap(avcodec_parameters_copy(
		outStream->codecpar,
		firstStream->codecpar));
	if (error) {
		LogError(u"avcodec_parameters_copy"_q, error);
		return {};
	}
	outStream->codecpar->codec_tag = 0;
	outStream->time_base = firstStream->time_base;

	error = AvErrorWrap(avformat_write_header(output.get(), nullptr));
	if (error) {
		LogError(u"avformat_write_header"_q, error);
		return {};
	}

	auto offsetPts = int64(0);
	auto durationPts = int64(0);
	auto lastPts = std::numeric_limits<int64>::min();
	auto lastDts = std::numeric_limits<int64>::min();
	auto copied = 0;

	const auto append = [&](
			not_null<AVFormatContext*> input,
			not_null<AVStream*> inStream) {
		auto firstPts = int64(AV_NOPTS_VALUE);
		auto firstDts = int64(AV_NOPTS_VALUE);
		auto sourceEndPts = offsetPts;

		auto packetWrap = Packet();
		auto &packet = packetWrap.fields();
		while (true) {
			auto error = AvErrorWrap(av_read_frame(input, &packet));
			if (error.code() == AVERROR_EOF) {
				break;
			} else if (error) {
				LogError(u"av_read_frame"_q, error);
				return false;
			}
			const auto guard = gsl::finally([&] {
				av_packet_unref(&packet);
			});
			if (packet.stream_index != inStream->index) {
				continue;
			}

			if (packet.pts != AV_NOPTS_VALUE) {
				if (firstPts == AV_NOPTS_VALUE) {
					firstPts = packet.pts;
				}
				packet.pts -= firstPts;
				if (packet.pts < 0) {
					packet.pts = 0;
				}
				packet.pts = av_rescale_q(
					packet.pts,
					inStream->time_base,
					outStream->time_base);
				packet.pts += offsetPts;
				if (packet.pts <= lastPts) {
					packet.pts = lastPts + 1;
				}
				lastPts = packet.pts;
			}
			if (packet.dts != AV_NOPTS_VALUE) {
				if (firstDts == AV_NOPTS_VALUE) {
					firstDts = packet.dts;
				}
				packet.dts -= firstDts;
				if (packet.dts < 0) {
					packet.dts = 0;
				}
				packet.dts = av_rescale_q(
					packet.dts,
					inStream->time_base,
					outStream->time_base);
				packet.dts += offsetPts;
				if (packet.dts <= lastDts) {
					packet.dts = lastDts + 1;
				}
				lastDts = packet.dts;
			}
			packet.duration = (packet.duration > 0)
				? av_rescale_q(
					packet.duration,
					inStream->time_base,
					outStream->time_base)
				: 0;

			const auto packetPosition = (packet.pts != AV_NOPTS_VALUE)
				? packet.pts
				: packet.dts;
			if (packetPosition != AV_NOPTS_VALUE) {
				const auto packetEnd = packetPosition
					+ std::max(int64(packet.duration), int64());
				durationPts = std::max(durationPts, packetEnd);
				sourceEndPts = std::max(sourceEndPts, packetEnd);
			}

			packet.stream_index = outStream->index;
			error = AvErrorWrap(av_interleaved_write_frame(output.get(), &packet));
			if (error) {
				LogError(u"av_interleaved_write_frame"_q, error);
				return false;
			}
			++copied;
		}

		offsetPts = sourceEndPts;
		return true;
	};
	if (!append(firstInput.get(), firstStream)
		|| !append(secondInput.get(), secondStream)) {
		return {};
	}
	if (!copied) {
		return {};
	}

	error = AvErrorWrap(av_write_trailer(output.get()));
	if (error) {
		LogError(u"av_write_trailer"_q, error);
		return {};
	}

	auto result = AudioEditResult();
	result.content = std::move(outputWrap.content);
	result.waveform = audioCountWaveform(Core::FileLocation(), result.content);
	result.duration = durationPts
		? PtsToTimeCeil(durationPts, outStream->time_base)
		: 0;
	return result;
}

} // namespace Media
