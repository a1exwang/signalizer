
#include "CSpectrum.h"
#include <cstdint>
#include <cpl/CMutex.h>
#include <cpl/Mathext.h>
#include "SignalizerDesign.h"
#include <cpl/rendering/OpenGLRasterizers.h>
#include <cpl/simd.h>
#include <cpl/dsp/filterdesign.h>
#include <array>

namespace Signalizer
{

	using namespace cpl;

	void CSpectrum::onGraphicsRendering(juce::Graphics & g)
	{
		// do software rendering
		if (!isOpenGL())
		{
			g.fillAll(state.colourBackground.withAlpha(1.0f));
			g.setColour(state.colourBackground.withAlpha(1.0f).contrasting());
			g.drawText("Enable OpenGL in settings to use the spectrum", getLocalBounds(), juce::Justification::centred);

			// post fps anyway
			auto tickNow = juce::Time::getHighResolutionTicks();
			avgFps.setNext(tickNow - lastFrameTick);
			lastFrameTick = tickNow;
		}
	}

	void CSpectrum::paint2DGraphics(juce::Graphics & g)
	{
		auto cStart = cpl::Misc::ClockCounter();

#pragma message cwarn("lel")

		// ------- draw frequency graph

		char buf[200];

		g.setColour(state.colourGrid.withMultipliedBrightness(0.5f));

		if (state.displayMode == DisplayMode::LineGraph)
		{
			auto complexScale = state.configuration == ChannelConfiguration::Complex ? 2.0f : 1.0f;
			g.setColour(state.colourGrid);
			const auto & divs = frequencyGraph.getDivisions();
			const auto & cdivs = complexFrequencyGraph.getDivisions();
			// text for frequency divisions
			for (auto & sdiv : divs)
			{
				sprintf_s(buf, "%.2f", sdiv.frequency);
				g.drawText(buf, float(complexScale * sdiv.coord) + 5, 20, 100, 20, juce::Justification::centredLeft);

			}
			// text for complex frequency divisions
			if (state.configuration == ChannelConfiguration::Complex)
			{
				auto normalizedScaleX = 1.0 / frequencyGraph.getBounds().dist();
				auto normXC = [=](double in) { return -static_cast<float>(normalizedScaleX * in * 2.0 - 1.0); };

				for (auto & sdiv : cdivs)
				{
					sprintf_s(buf, "-i*%.2f", sdiv.frequency);
					// transform back and forth from unit cartesion... should insert a TODO here.
					g.drawText(buf, getWidth() * (normXC(sdiv.coord) + 1) * 0.5 + 5, 20, 100, 20, juce::Justification::centredLeft);
				}
			}
			// text for db divisions
			for (auto & dbDiv : dbGraph.getDivisions())
			{
				sprintf_s(buf, "%.2f", dbDiv.dbVal);
				g.drawText(buf, 5, float(dbDiv.coord), 100, 20, juce::Justification::centredLeft);
			}

			

		}
		else
		{
			float height = getHeight();
			float baseWidth = getWidth() * 0.05f;
			
			float gradientOffset = 10.0f;

			g.setColour(state.colourGrid);
			const auto & divs = frequencyGraph.getDivisions();


			for (auto & sdiv : divs)
			{
				sprintf_s(buf, "%.2f", sdiv.frequency);
				g.drawText(buf, gradientOffset + baseWidth + 5, float(height - sdiv.coord) - 10 /* height / 2 */, 100, 20, juce::Justification::centredLeft);
			}


			// draw gradient

			juce::ColourGradient gradient;

			// fill in colours

			double gradientPos = 0.0;

			for (int i = 0; i < numSpectrumColours + 1; i++)
			{
				gradientPos += state.normalizedSpecRatios[i];
				gradient.addColour(gradientPos, state.colourSpecs[i].toJuceColour());
			}


			gradient.point1 = {gradientOffset * 0.5f, (float)getHeight() };
			gradient.point2 = {gradientOffset * 0.5f, 0.0f };

			g.setGradientFill(gradient);

			g.fillRect(0.0f, 0.0f, gradientOffset, (float)getHeight());
		}

		drawFrequencyTracking(g);

		if (kdiagnostics.bGetValue() > 0.5)
		{
			char text[1000];
			auto fps = 1.0 / (avgFps.getAverage() / juce::Time::getHighResolutionTicksPerSecond());

			auto totalCycles = renderCycles + cpl::Misc::ClockCounter() - cStart;
			double cpuTime = (double(totalCycles) / (processorSpeed * 1000 * 1000) * 100) * fps;
			double asu = 100 * audioStream.getPerfMeasures().asyncUsage.load(std::memory_order_relaxed);
			double aso = 100 * audioStream.getPerfMeasures().asyncOverhead.load(std::memory_order_relaxed);
			g.setColour(juce::Colours::blue);
			sprintf(text, "%dx%d {%.3f, %.3f}: %.1f fps - %.1f%% cpu, deltaG = %.4f, deltaO = %.4f (rt: %.2f%% - %.2f%%, d: %llu), (as: %.2f%% - %.2f%%)",
				getWidth(), getHeight(), state.viewRect.left, state.viewRect.right,
				fps, cpuTime, graphicsDeltaTime(), openGLDeltaTime(),
				100 * audioStream.getPerfMeasures().rtUsage.load(std::memory_order_relaxed),
				100 * audioStream.getPerfMeasures().rtOverhead.load(std::memory_order_relaxed),
				audioStream.getPerfMeasures().droppedAudioFrames.load(std::memory_order_relaxed),
				asu,
				aso);
			
			g.drawSingleLineText(text, 10, 20);

		}
	}

	void CSpectrum::drawFrequencyTracking(juce::Graphics & g)
	{
		auto graphN = newc.frequencyTrackingGraph.load(std::memory_order_acquire);
		if (graphN == LineGraphs::None)
			return;


		double
			mouseFrequency = 0,
			mouseDBs = 0,
			peakFrequency = 0,
			peakDBs = 0,
			mouseFraction = 0,
			mouseFractionOrth = 0,
			peakFraction = 0,
			peakFractionY = 0,
			peakX = 0,
			peakY = 0,
			mouseX = 0,
			mouseY = 0,
			adjustedScallopLoss = scallopLoss,
			peakDeviance = 0,
			sampleRate = audioStream.getInfo().sampleRate.load(std::memory_order_relaxed),
			maxFrequency = sampleRate / 2;

		bool frequencyIsComplex = false;

		char buf[1000];

		double viewSize = state.viewRect.dist();

		if (state.displayMode == DisplayMode::LineGraph)
		{
			mouseX = cmouse.x.load(std::memory_order_acquire);
			mouseY = cmouse.y.load(std::memory_order_acquire);


			g.drawLine(static_cast<float>(mouseX), 0, static_cast<float>(mouseX), getHeight(), 1);
			g.drawLine(0, static_cast<float>(mouseY), getWidth(), static_cast<float>(mouseY), 1);

			mouseFraction = mouseX / (getWidth() - 1);
			mouseFractionOrth = mouseY / (getHeight() - 1);

			if (state.viewScale == ViewScaling::Logarithmic)
			{
				if (state.configuration != ChannelConfiguration::Complex)
				{
					mouseFrequency = state.minLogFreq * std::pow(maxFrequency / state.minLogFreq, state.viewRect.left + viewSize * mouseFraction);
				}
				else
				{
					auto arg = state.viewRect.left + viewSize * mouseFraction;
					if(arg < 0.5)
						mouseFrequency = state.minLogFreq * std::pow(maxFrequency / state.minLogFreq, arg * 2);
					else
					{
						arg -= 0.5;
						auto power = state.minLogFreq * std::pow(maxFrequency / state.minLogFreq, 1.0 - arg * 2);
						mouseFrequency = power;
						frequencyIsComplex = true;
					}
				}
			}
			else
			{
				auto arg = state.viewRect.left + viewSize * mouseFraction;
				if (state.configuration != ChannelConfiguration::Complex)
				{
					mouseFrequency = arg * maxFrequency;
				}
				else
				{
					if (arg < 0.5)
					{
						mouseFrequency = 2 * arg * maxFrequency;
					}
					else
					{
						arg -= 0.5;
						mouseFrequency = (1.0 - arg * 2) * maxFrequency;
						frequencyIsComplex = true;
					}
				}
			}

			mouseDBs = cpl::Math::UnityScale::linear(mouseFractionOrth, getDBs().high, getDBs().low); // y coords are flipped..
		}
		else
		{
			// ?
		}

		mouseFraction = cpl::Math::confineTo(mouseFraction, 0, 1);

		// calculate nearest peak
		double const nearbyFractionToConsider = 0.03;

		auto precisionError = 0.001;
		auto interpolationError = 0.01;

		// TODO: these special cases can be handled (on a rainy day)
		if (state.configuration == ChannelConfiguration::Complex || !(state.algo == TransformAlgorithm::FFT && graphN == LineGraphs::Transform))
		{

			if (graphN == LineGraphs::Transform)
				graphN = LineGraphs::LineMain;

			auto & results = lineGraphs[graphN].results;
			auto N = results.size();
			auto pivot = cpl::Math::round<std::size_t>(N * mouseFraction);
			auto range = cpl::Math::round<std::size_t>(N * nearbyFractionToConsider);

			auto lowerBound = range > pivot ? 0 : pivot - range;
			auto higherBound = range + pivot > N ? N : range + pivot;

			auto peak = std::max_element(results.begin() + lowerBound, results.begin() + higherBound, 
				[](auto & left, auto & right) { return left.leftMagnitude < right.leftMagnitude; });

			// scan for continuously rising peaks at boundaries
			if (peak == results.begin() + lowerBound && lowerBound != 0)
			{
				while (true)
				{
					auto nextPeak = peak - 1;
					if (nextPeak == results.begin())
						break;
					else if (nextPeak->leftMagnitude < peak->leftMagnitude)
						break;
					else
						peak = nextPeak;
				}
			}
			else if (peak == results.begin() + (higherBound - 1))
			{
				while (true)
				{
					auto nextPeak = peak + 1;
					if (nextPeak == results.end())
						break;
					else if (nextPeak->leftMagnitude < peak->leftMagnitude)
						break;
					else
						peak = nextPeak;
				}
			}

			auto peakOffset = std::distance(results.begin(), peak);

			// interpolate using a parabolic fit
			// https://ccrma.stanford.edu/~jos/parshl/Peak_Detection_Steps_3.html

			peakFrequency = mappedFrequencies[peakOffset];


			auto offsetIsEnd = peakOffset == mappedFrequencies.size() - 1;
			peakDeviance = mappedFrequencies[offsetIsEnd ? peakOffset : peakOffset + 1] - mappedFrequencies[offsetIsEnd ? peakOffset - 1 : peakOffset];

			if (state.algo == TransformAlgorithm::FFT)
			{
				// non-smooth interpolations suffer from peak detection losses
				if(state.binPolation != BinInterpolation::Lanczos)
					peakDeviance = std::max(peakDeviance, 0.5 * getFFTSpace<std::complex<fftType>>() / N);
			}

			peakX = peakOffset;
			peakFractionY = results[peakOffset].leftMagnitude;
			peakY = getHeight() - peakFractionY * getHeight();

			peakDBs = cpl::Math::UnityScale::linear(peakFractionY, getDBs().low, getDBs().high);

			adjustedScallopLoss = 20 * std::log10(scallopLoss - precisionError * 0.1);

		}
		else
		{
			// search the original FFT
			auto N = getFFTSpace<std::complex<fftType>>();
			auto points = getNumFilters();
			auto lowerBound = cpl::Math::round<cpl::ssize_t>(points * (mouseFraction - nearbyFractionToConsider));
			lowerBound = cpl::Math::round<cpl::ssize_t>((N * mappedFrequencies[cpl::Math::confineTo(lowerBound, 0, points - 1)] / sampleRate));
			auto higherBound = cpl::Math::round<cpl::ssize_t>(points * (mouseFraction + nearbyFractionToConsider));
			higherBound = cpl::Math::round<cpl::ssize_t>((N * mappedFrequencies[cpl::Math::confineTo(higherBound, 0, points - 1)] / sampleRate));

			lowerBound = cpl::Math::confineTo(lowerBound, 0, N);
			higherBound = cpl::Math::confineTo(higherBound, 0, N);

			auto source = getAudioMemory<std::complex<fftType>>();

			auto peak = std::max_element(source + lowerBound, source + higherBound, 
				[](auto & left, auto & right) { return cpl::Math::square(left) < cpl::Math::square(right); });

			// scan for continuously rising peaks at boundaries
			if (peak == source + lowerBound && lowerBound != 0)
			{
				while (true)
				{
					auto nextPeak = peak - 1;
					if (nextPeak == source)
						break;
					else if (cpl::Math::square(*nextPeak) < cpl::Math::square(*peak))
						break;
					else
						peak = nextPeak;
				}
			}
			else if (peak == source + (higherBound - 1))
			{
				while (true)
				{
					auto nextPeak = peak + 1;
					if (nextPeak == source + N)
						break;
					else if (cpl::Math::square(*nextPeak) < cpl::Math::square(*peak))
						break;
					else
						peak = nextPeak;
				}
			}

			auto peakOffset = std::distance(source, peak);

			auto const invSize = windowScale / (getWindowSize() * 0.5);

			// interpolate using a parabolic fit
			// https://ccrma.stanford.edu/~jos/parshl/Peak_Detection_Steps_3.html
			auto alpha = 20 * std::log10(std::abs(source[peakOffset == 0 ? 0 : peakOffset - 1] * invSize));
			auto beta = 20 * std::log10(std::abs(source[peakOffset] * invSize));
			auto gamma = 20 * std::log10(std::abs(source[peakOffset == N ? peakOffset : peakOffset + 1] * invSize));

			auto phi = 0.5 * (alpha - gamma) / (alpha - 2 * beta + gamma);

			// global
			peakFraction = 2 * (peakOffset + phi) / double(N);
			peakFrequency = 0.5 * peakFraction * sampleRate;
			// translate to local

			peakDBs = beta - 0.25*(alpha - gamma) * phi;

			peakX = frequencyGraph.fractionToCoordTransformed(peakFraction);
			peakY = cpl::Math::UnityScale::Inv::linear(peakDBs, getDBs().low, getDBs().high);

			peakY = getHeight() - peakY * getHeight();


			// deviance firstly considers bin width in frequency, scaled by bin position (precision gets better as 
			// frequency increases) and scaled by assumed precision interpolation
			auto normalizedDeviation = (1.0 - peakFraction) / N;
			peakDeviance = 2 * interpolationError * normalizedDeviation * sampleRate + precisionError;
			adjustedScallopLoss = 1.0 - ((1.0 - scallopLoss) * interpolationError + normalizedDeviation); // subtract error
			adjustedScallopLoss = 20 * std::log10(adjustedScallopLoss - precisionError * 0.2);
		}

		// draw a line to the peak from the mouse
		if (std::isnormal(peakX) && std::isnormal(peakY))
		{
			g.drawLine((float)mouseX, (float)mouseY, (float)peakX, (float)peakY, 1.5f);

			// this mechanism, although technically delayed by a frame, avoids recalculating
			// the costly scalloping loss each frame.
			auto truncatedPeak = (int)peakX;
			if (truncatedPeak != lastPeak)
			{
				scallopLoss = getScallopingLossAtCoordinate(truncatedPeak);
				lastPeak = truncatedPeak;
			}

		}

		// adjust for complex things
		bool peakIsComplex = peakFrequency > sampleRate * 0.5;
		if (peakIsComplex)
			peakFrequency = sampleRate - peakFrequency;

		// TODO: calculate at runtime, at some point.
		double textOffset[2] = { 20, -85 };
		double estimatedSize[2] = { peakIsComplex || frequencyIsComplex ? 155 : 145, 85 };

		if (peakDBs > 1000)
			peakDBs = std::numeric_limits<double>::infinity();
		else if(peakDBs < -1000)
			peakDBs = -std::numeric_limits<double>::infinity();

		// TODO: use a monospace font for this part
		sprintf_s(buf, u8"+x: \t%s%.5f Hz\n+y: \t%.5f dB\n\u039Bx: \t%s%.5f Hz\n\u039B~:\t%.3f Hz\u03C3\n\u039By: \t%.5f dB\n\u039BSL: \t+%.3f dB\u03C3 ", 
			frequencyIsComplex ? "-i*" : "", mouseFrequency, mouseDBs, peakIsComplex ? "-i*" : "", peakFrequency, peakDeviance, peakDBs, -adjustedScallopLoss);

		// render text rectangle
		auto xpoint = mouseX + textOffset[0] ;
		if (xpoint + estimatedSize[0] > getWidth())
			xpoint = mouseX - (textOffset[0] + estimatedSize[0]);

		auto ypoint = mouseY + textOffset[1];
		if (ypoint  < 0)
			ypoint = mouseY + (textOffset[0]);

		juce::Rectangle<float> rect { (float)xpoint, (float)ypoint, (float)estimatedSize[0], (float)estimatedSize[1] };

		auto rectInside = rect.withSizeKeepingCentre(estimatedSize[0] * 0.95f, estimatedSize[1] * 0.95f).toType<int>();

		// clear background
		// TODO: add a nice semi transparent effect!
		g.setColour(state.colourBackground);
		g.fillRoundedRectangle(rect, 2);


		// reset colour
		g.setColour(state.colourGrid.withMultipliedBrightness(1.1f));
		g.drawRoundedRectangle(rect, 2, 0.7f);

		g.drawFittedText(juce::CharPointer_UTF8(buf), rectInside, juce::Justification::centredLeft, 6);

	}

	void CSpectrum::initOpenGL()
	{
		const int imageSize = 128;
		const float fontToPixelScale = 90 / 64.0f;

		//oglImage.resize(getWidth(), getHeight(), false);
		flags.openGLInitiation = true;
		textures.clear();

	}

	void CSpectrum::closeOpenGL()
	{
		textures.clear();
		oglImage.offload();
	}


	template<std::size_t N, std::size_t V, cpl::GraphicsND::ComponentOrder order>
		void ColourScale2(cpl::GraphicsND::UPixel<order> * __RESTRICT__ outPixel, 
			float intensity, cpl::GraphicsND::UPixel<order> colours[N + 1], float normalizedScales[N])
		{
			intensity = cpl::Math::confineTo(intensity, 0.0f, 1.0f);
			
			float accumulatedSum = 0.0f;

			for (std::size_t i = 0; i < N; ++i)
			{
				accumulatedSum += normalizedScales[i];

				if (accumulatedSum >= intensity)
				{
					std::uint16_t factor = cpl::Math::round<std::uint8_t>(0xFF * cpl::Math::UnityScale::Inv::linear<float>(intensity, accumulatedSum - normalizedScales[i], accumulatedSum));

					std::size_t 
						x1 = std::max(signed(i) - 1, 0), 
						x2 = std::min(x1 + 1, N - 1);

					for (std::size_t p = 0; p < V; ++p)
					{
						outPixel->pixel.data[p] = (((colours[x1].pixel.data[p] * (0x00FF - factor)) + 0x80) >> 8) + (((colours[x2].pixel.data[p] * factor) + 0x80) >> 8);
					}

					return;
				}
			}
		}

	void ColorScale(uint8_t * pixel, float intensity)
	{

		uint8_t red = 0, blue = 0, green = 0;
		// set blue

		if (intensity <= 0)
		{
		}
		else if (intensity < 0.16666f && intensity > 0)
		{
			blue = 6 * intensity * 0x7F;
		}
		else if (intensity < 0.3333f)
		{
			red = 6 * (intensity - 0.16666f) * 0xFF;
			blue = 0x7F - (red >> 1);

		}
		else if (intensity < 0.6666f)
		{
			red = 0xFF;
			green = 3 * (intensity - 0.3333) * 0xFF;
		}
		else if (intensity < 1)
		{
			red = 0xFF;
			green = 0xFF;
			blue = 3 * (intensity - 0.66666) * 0xFF;
		}
		else
			red = green = blue = 0xFF;
		// set green
		pixel[0] = red;
		pixel[1] = green;
		pixel[2] = blue;
		// set red

		// saturate


	}

	void CSpectrum::onOpenGLRendering()
	{
		auto cStart = cpl::Misc::ClockCounter();
		// starting from a clean slate?
		CPL_DEBUGCHECKGL();

		for (std::size_t i = 0; i < LineGraphs::LineEnd; ++i)
			lineGraphs[i].filter.setSampleRate(fpoint(1.0 / openGLDeltaTime()));

		bool lineTransformReady = false;

		// lock the memory buffers, and do our thing.
		{
			handleFlagUpdates();
			// line graph data for ffts are rendered now.
			if (state.displayMode == DisplayMode::LineGraph)
				lineTransformReady = prepareTransform(audioStream.getAudioBufferViews());
		}
		// flags may have altered ogl state
		CPL_DEBUGCHECKGL();



		juce::OpenGLHelpers::clear(state.colourBackground);

		cpl::OpenGLEngine::COpenGLStack openGLStack;
		// set up openGL
		//openGLStack.setBlender(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		openGLStack.loadIdentityMatrix();
		CPL_DEBUGCHECKGL();

		state.antialias ? openGLStack.enable(GL_MULTISAMPLE) : openGLStack.disable(GL_MULTISAMPLE);
		CPL_DEBUGCHECKGL();

		openGLStack.setLineSize(std::max(0.001f, state.primitiveSize * 10));
		openGLStack.setPointSize(std::max(0.001f, state.primitiveSize * 10));
		CPL_DEBUGCHECKGL();

		CPL_DEBUGCHECKGL();

		/// <summary>
		/// 
		/// </summary>
		/// 
		///



		switch (state.displayMode)
		{
		case DisplayMode::LineGraph:
			// no need to lock in this case, as this display mode is exclusively switched,
			// such that only we have access to it.
			if (lineTransformReady)
			{
				doTransform();
				mapToLinearSpace();
				postProcessStdTransform();
			}
			renderLineGraph<Types::v8sf>(openGLStack); break;
		case DisplayMode::ColourSpectrum:
			// mapping and processing is already done here.
			renderColourSpectrum<Types::v8sf>(openGLStack); break;

		}




		renderCycles = cpl::Misc::ClockCounter() - cStart;
		auto tickNow = juce::Time::getHighResolutionTicks();
		avgFps.setNext(tickNow - lastFrameTick);
		lastFrameTick = tickNow;
		CPL_DEBUGCHECKGL();
		renderGraphics([&](juce::Graphics & g) { paint2DGraphics(g); });
		CPL_DEBUGCHECKGL();
	}


	template<typename V>
		void CSpectrum::renderColourSpectrum(cpl::OpenGLEngine::COpenGLStack & ogs)
		{
			CPL_DEBUGCHECKGL();
			auto pW = oglImage.getWidth();
			if (!pW)
				return;

			if (!state.isFrozen)
			{

				framePixelPosition %= pW;
				double bufferSmoothing = state.bufferSmoothing;
				auto approximateFrames = getApproximateStoredFrames();
				/*if (approximateFrames == 0)
					approximateFrames = framesPerUpdate;*/
				std::size_t processedFrames = 0;
				framesPerUpdate = approximateFrames + bufferSmoothing * (framesPerUpdate - approximateFrames);
				auto framesThisTime = cpl::Math::round<std::size_t>(framesPerUpdate);

				// if there's no buffer smoothing at all, we just capture every frame possible.
				// 
				bool shouldCap = state.bufferSmoothing != 0.0;

				while ((!shouldCap || (processedFrames++ < framesThisTime)) && processNextSpectrumFrame())
				{
#pragma message cwarn("Update frames per update each time inside here, but as a local variable! There may come more updates meanwhile.")
					// run the next frame through pixel filters and format it etc.

					for (int i = 0; i < getAxisPoints(); ++i)
					{
						ColourScale2<numSpectrumColours + 1, 4>(columnUpdate.data() + i, lineGraphs[LineGraphs::LineMain].results[i].magnitude, state.colourSpecs, state.normalizedSpecRatios);
					}
					//CPL_DEBUGCHECKGL();
					oglImage.updateSingleColumn(framePixelPosition, columnUpdate, GL_RGBA);
					//CPL_DEBUGCHECKGL();

					framePixelPosition++;
					framePixelPosition %= pW;
				}
			}

			CPL_DEBUGCHECKGL();

			{
				cpl::OpenGLEngine::COpenGLImage::OpenGLImageDrawer imageDrawer(oglImage, ogs);

				imageDrawer.drawCircular((float)((double)(framePixelPosition) / (pW - 1)));
				
			}

			CPL_DEBUGCHECKGL();

			// render grid
			{
				auto normalizedScale = 1.0 / getHeight();

				// draw vertical lines.
				const auto & lines = frequencyGraph.getLines();

				auto norm = [=](double in) { return static_cast<float>(normalizedScale * in * 2.0 - 1.0); };

				float baseWidth = 0.1f;

				float gradientOffset = 10.0f / getWidth() - 1.0f;

				OpenGLEngine::PrimitiveDrawer<128> lineDrawer(ogs, GL_LINES);

				lineDrawer.addColour(state.colourGrid.withMultipliedBrightness(0.5f));

				for (auto dline : lines)
				{
					auto line = norm(dline);
					lineDrawer.addVertex(gradientOffset, line, 0.0f);
					lineDrawer.addVertex(gradientOffset + baseWidth * 0.7f, line, 0.0f);
				}

				lineDrawer.addColour(state.colourGrid);
				const auto & divs = frequencyGraph.getDivisions();

				for (auto & sdiv : divs)
				{
					auto line = norm(sdiv.coord);
					lineDrawer.addVertex(gradientOffset, line, 0.0f);
					lineDrawer.addVertex(gradientOffset + baseWidth, line, 0.0f);
				}

			}
			CPL_DEBUGCHECKGL();
		}


	template<typename V>
	void CSpectrum::renderLineGraph(cpl::OpenGLEngine::COpenGLStack & ogs)
	{
		ogs.setBlender(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
		int points = getAxisPoints() - 1;

		for (std::size_t k = 0; k < LineGraphs::LineEnd; ++k)
		{
			switch (state.configuration)
			{
			case ChannelConfiguration::MidSide:
			case ChannelConfiguration::Phase:
			case ChannelConfiguration::Separate:
			{
				OpenGLEngine::PrimitiveDrawer<256> lineDrawer(ogs, GL_LINE_STRIP);
				lineDrawer.addColour(state.colourTwo[k]);
				for (int i = 0; i < (points + 1); ++i)
				{
					lineDrawer.addVertex((float(i) / points) * 2 - 1, lineGraphs[k].results[i].rightMagnitude * 2 - 1, -0.5);
				}
			}
			// (fall-through intentional)
			case ChannelConfiguration::Left:
			case ChannelConfiguration::Right:
			case ChannelConfiguration::Merge:
			case ChannelConfiguration::Side:
			case ChannelConfiguration::Complex:
			{
				OpenGLEngine::PrimitiveDrawer<256> lineDrawer(ogs, GL_LINE_STRIP);
				lineDrawer.addColour(state.colourOne[k]);
				for (int i = 0; i < (points + 1); ++i)
				{
					lineDrawer.addVertex((float(i) / points) * 2 - 1, lineGraphs[k].results[i].leftMagnitude * 2 - 1, 0);
				}
			}
			default:
				break;
			}
		}
		// render grid
		{
			OpenGLEngine::PrimitiveDrawer<128> lineDrawer(ogs, GL_LINES);

			lineDrawer.addColour(state.colourGrid.withMultipliedBrightness(0.5f));

			auto xDist = frequencyGraph.getBounds().dist();
			auto normalizedScaleX = 1.0 / xDist;
			auto normalizedScaleY = 1.0 / getHeight();
			// draw vertical lines.
			const auto & lines = frequencyGraph.getLines();
			const auto & clines = complexFrequencyGraph.getLines();
			// TODO: fix using a matrix modification instead (cheaper)
			auto normX = [=](double in) { return static_cast<float>(normalizedScaleX * in * 2.0 - 1.0); };
			auto normXC = [=](double in) { return -static_cast<float>(normalizedScaleX * in * 2.0 - 1.0); };
			//auto normXC = normX;
			auto normY = [=](double in) {  return static_cast<float>(1.0 - normalizedScaleY * in * 2.0); };



			for (auto dline : lines)
			{
				auto line = normX(dline);
				lineDrawer.addVertex(line, -1.0f, 0.0f);
				lineDrawer.addVertex(line, 1.0f, 0.0f);
			}

			for (auto dline : clines)
			{
				auto line = normXC(dline);
				lineDrawer.addVertex(line, -1.0f, 0.0f);
				lineDrawer.addVertex(line, 1.0f, 0.0f);
			}

			//m.scale(1, getHeight(), 1);
			lineDrawer.addColour(state.colourGrid);
			const auto & divs = frequencyGraph.getDivisions();
			const auto & cdivs = complexFrequencyGraph.getDivisions();

			for (auto & sdiv : divs)
			{
				auto line = normX(sdiv.coord);
				lineDrawer.addVertex(line, -1.0f, 0.0f);
				lineDrawer.addVertex(line, 1.0f, 0.0f);
			}

			for (auto & sdiv : cdivs)
			{
				auto line = normXC(sdiv.coord);
				lineDrawer.addVertex(line, -1.0f, 0.0f);
				lineDrawer.addVertex(line, 1.0f, 0.0f);
			}

			// draw horizontal lines:
			for (auto & dbDiv : dbGraph.getDivisions())
			{
				auto line = normY(dbDiv.coord);
				lineDrawer.addVertex(-1.0f, line, 0.0f);
				lineDrawer.addVertex(1.0f, line, 0.0f);
			}
		}



	}

};
