/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cassert>
#include <deque>

#include "ProfileDrawer.h"
#include "InputReceiver.h"
#include "Game/GlobalUnsynced.h"
#include "Lua/LuaAllocState.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/GlobalRenderingInfo.h"
#include "Rendering/GL/RenderBuffers.h"
#include "Sim/Features/FeatureMemPool.h"
#include "Sim/Misc/GlobalConstants.h" // for GAME_SPEED
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/Path/IPathManager.h"
#include "Sim/Units/UnitMemPool.h"
#include "Sim/Projectiles/ProjectileHandler.h"
#include "Sim/Projectiles/ProjectileMemPool.h"
#include "Sim/Weapons/WeaponMemPool.h"
#include "System/EventHandler.h"
#include "System/TimeProfiler.h"
#include "System/Threading/ThreadPool.h"
#include "System/SafeUtil.h"
#include "lib/lua/include/LuaUser.h" // spring_lua_alloc_get_stats

#include "System/Misc/TracyDefs.h"

ProfileDrawer* ProfileDrawer::instance = nullptr;

static constexpr float MAX_FRAMES_HIST_TIME = 0.5f; // secs

static constexpr float  MIN_X_COOR = 0.6f;
static constexpr float  MAX_X_COOR = 0.99f;
static constexpr float  MIN_Y_COOR = 0.95f;
static constexpr float LINE_HEIGHT = 0.013f;

static constexpr unsigned int DBG_FONT_FLAGS = (FONT_SCALE | FONT_NORM | FONT_SHADOW);

typedef std::pair<spring_time, spring_time> TimeSlice;
static std::deque<TimeSlice> vidFrames;
static std::deque<TimeSlice> simFrames;
static std::deque<TimeSlice> lgcFrames;
static std::deque<TimeSlice> swpFrames;
static std::deque<TimeSlice> uusFrames;


ProfileDrawer::ProfileDrawer()
: CEventClient("[ProfileDrawer]", 199991, false)
{
	autoLinkEvents = true;
	RegisterLinkedEvents(this);
	eventHandler.AddClient(this);
}

void ProfileDrawer::SetEnabled(bool enable)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!enable) {
		spring::SafeDelete(instance);
		return;
	}

	assert(instance == nullptr);
	instance = new ProfileDrawer();

	// reset peak indicators each time the drawer is restarted
	CTimeProfiler::GetInstance().ResetPeaks();
}



static void DrawBufferStats(const float2 pos)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const float4 drawArea = {pos.x, pos.y + 0.02f, 0.2f, pos.y - (0.23f + 0.02f)};

	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();

	// background
	constexpr SColor bgColor = SColor{ 0.0f, 0.0f, 0.0f, 0.5f };
	rb.AddVertex({{drawArea.x - 10.0f * globalRendering->pixelX, drawArea.y - 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // TL
	rb.AddVertex({{drawArea.x - 10.0f * globalRendering->pixelX, drawArea.w + 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // BL
	rb.AddVertex({{drawArea.z + 10.0f * globalRendering->pixelX, drawArea.w + 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // BR

	rb.AddVertex({{drawArea.z + 10.0f * globalRendering->pixelX, drawArea.w + 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // BR
	rb.AddVertex({{drawArea.z + 10.0f * globalRendering->pixelX, drawArea.y - 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // TR
	rb.AddVertex({{drawArea.x - 10.0f * globalRendering->pixelX, drawArea.y - 10.0f * globalRendering->pixelY, 0.0f}, bgColor }); // TL
	rb.Submit(GL_TRIANGLES);

	{
		std::array<size_t, 4> lfMetrics = {0};
		std::array<size_t, 8> tfMetrics = {0};
		for (const auto& ft : CglFont::GetLoadedFonts()) {
			if (ft.expired())
				continue;

			auto lf = std::static_pointer_cast<CglFont>(ft.lock());
			lf->GetStats(tfMetrics);
			lfMetrics[0] += tfMetrics[0];
			lfMetrics[1] += tfMetrics[1];
			lfMetrics[2] += tfMetrics[2];
			lfMetrics[3] += tfMetrics[3];

			lfMetrics[0] += tfMetrics[4];
			lfMetrics[1] += tfMetrics[5];
			lfMetrics[2] += tfMetrics[6];
			lfMetrics[3] += tfMetrics[7];
		}

		static constexpr const char* FMT = "\t%s={e=%u i=%u subs{e,i}={%u,%u}}";
		font->SetTextColor(1.0f, 1.0f, 0.5f, 0.8f);

		font->glFormat(pos.x, pos.y - 0.005f, 0.5f, FONT_TOP | DBG_FONT_FLAGS | FONT_BUFFERED, FMT, "FONTS", lfMetrics[0], lfMetrics[1], lfMetrics[2], lfMetrics[3]);

		float bias = 0.0f;
		for (const auto& rb : RenderBuffer::GetAllStandardRenderBuffers()) {
			const auto ms = rb->GetMaxSize();
			if (ms[0] == 0 && ms[1] == 0)
				continue;

			std::string_view rbNameShort = rb->GetBufferName();
			const auto uId = rbNameShort.find_last_of("_");
			rbNameShort = rbNameShort.substr(uId + 1, rbNameShort.size() - uId - 1);

			font->glFormat(pos.x, pos.y - (0.025f + bias), 0.5f, FONT_TOP | DBG_FONT_FLAGS | FONT_BUFFERED, FMT, rbNameShort,
				rb->SumElems(), rb->SumIndcs(),
				rb->NumSubmits(false), rb->NumSubmits(true)
			);
			bias += 0.02f;
		}

		#undef FMT
	}
}

static void DrawTimeSlices(
	std::deque<TimeSlice>& frames,
	const spring_time maxTime,
	const float4& drawArea,
	const float4& sliceColor
) {
	RECOIL_DETAILED_TRACY_ZONE;
	const float y1 = drawArea.y;
	const float y2 = drawArea.w;

	// render
	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	const SColor sliceCol = SColor{ sliceColor };

	for (const TimeSlice& ts: frames) {
		float x1 = (ts.first  % maxTime).toSecsf() / maxTime.toSecsf();
		float x2 = (ts.second % maxTime).toSecsf() / maxTime.toSecsf();

		x2 = std::max(x1 + globalRendering->pixelX, x2);

		x1 = drawArea.x + x1 * (drawArea.z - drawArea.x);
		x2 = drawArea.x + x2 * (drawArea.z - drawArea.x);

		rb.AddVertex({{x1, y1, 0.0f}, sliceCol}); // tl
		rb.AddVertex({{x1, y2, 0.0f}, sliceCol}); // bl
		rb.AddVertex({{x2, y2, 0.0f}, sliceCol}); // br

		rb.AddVertex({{x2, y2, 0.0f}, sliceCol}); // br
		rb.AddVertex({{x2, y1, 0.0f}, sliceCol}); // tr
		rb.AddVertex({{x1, y1, 0.0f}, sliceCol}); // tl

		const float mx1 = x1 + 3.0f * globalRendering->pixelX;
		const float mx2 = x2 - 3.0f * globalRendering->pixelX;

		if (mx1 >= mx2)
			continue;

		rb.AddVertex({{mx1, y1 + 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // bl
		rb.AddVertex({{mx1, y2 - 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // tl
		rb.AddVertex({{mx2, y2 - 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // tr

		rb.AddVertex({{mx2, y2 - 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // tr
		rb.AddVertex({{mx2, y1 + 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // br
		rb.AddVertex({{mx1, y1 + 3.0f * globalRendering->pixelX, 0.0f}, sliceCol}); // bl
	}
}


static void DrawThreadBarcode(TypedRenderBuffer<VA_TYPE_C   >& rb)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& profiler = CTimeProfiler::GetInstance();
	constexpr SColor    barColor = SColor{0.0f, 0.0f, 0.0f, 0.5f};
	constexpr SColor feederColor = SColor{1.0f, 0.0f, 0.0f, 1.0f};

	const float drawArea[4] = {0.01f, 0.30f, (MIN_X_COOR * 0.5f), 0.35f};

	const spring_time curTime = spring_now();
	const spring_time maxTime = spring_secs(MAX_THREAD_HIST_TIME);

	const size_t numThreads = std::min(profiler.GetNumThreadProfiles(), (size_t)ThreadPool::GetNumThreads());

	{
		// background
		rb.AddVertex({{drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // tl
		rb.AddVertex({{drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // bl
		rb.AddVertex({{drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // br

		rb.AddVertex({{drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // br
		rb.AddVertex({{drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // tr
		rb.AddVertex({{drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor}); // tl
	}
	{
		// title
		font->glFormat(drawArea[0], drawArea[3], 0.7f, FONT_TOP | DBG_FONT_FLAGS | FONT_BUFFERED, "ThreadPool (%.1f seconds :: %u threads)", MAX_THREAD_HIST_TIME, numThreads);
	}
	{
		// Need to lock; CleanupOldThreadProfiles pop_front()'s old entries
		// from threadProf while ~ScopedMtTimer can modify it concurrently.
		profiler.ToggleLock(true);
		profiler.CleanupOldThreadProfiles();

		// bars for each pool-thread profile
		// Create a virtual row at the top to give some space to see the threads without the title getting in the way.
		size_t i = 0;
		size_t numRows = numThreads + 1;
		for (auto& threadProf: profiler.GetThreadProfiles()) {
			if (i >= numThreads) break;
			float drawArea2[4] = {drawArea[0], 0.0f, drawArea[2], 0.0f};
			drawArea2[1] = drawArea[1] + ((drawArea[3] - drawArea[1]) / numRows) * i++;
			drawArea2[3] = drawArea[1] + ((drawArea[3] - drawArea[1]) / numRows) * i - (4 * globalRendering->pixelY);
			DrawTimeSlices(threadProf, maxTime, drawArea2, {1.0f, 0.0f, 0.0f, 0.6f});
		}

		profiler.ToggleLock(false);
	}
	{
		// feeder
		const float r = (curTime % maxTime).toSecsf() / MAX_THREAD_HIST_TIME;
		const float xf = drawArea[0] + r * (drawArea[2] - drawArea[0]);

		rb.AddVertex({{xf                                 , drawArea[1], 0.0f}, feederColor}); // tl
		rb.AddVertex({{xf                                 , drawArea[3], 0.0f}, feederColor}); // bl
		rb.AddVertex({{xf + 5.0f * globalRendering->pixelX, drawArea[3], 0.0f}, feederColor}); // br

		rb.AddVertex({{xf + 5.0f * globalRendering->pixelX, drawArea[3], 0.0f}, feederColor}); // br
		rb.AddVertex({{xf + 5.0f * globalRendering->pixelX, drawArea[1], 0.0f}, feederColor}); // tr
		rb.AddVertex({{xf                                 , drawArea[1], 0.0f}, feederColor}); // tl
	}
}


static void DrawFrameBarcode(TypedRenderBuffer<VA_TYPE_C   >& rb)
{
	RECOIL_DETAILED_TRACY_ZONE;
	constexpr SColor    barColor = SColor{0.0f, 0.0f, 0.0f, 0.5f};
	constexpr SColor feederColor = SColor{1.0f, 0.0f, 0.0f, 1.0f};

	const float drawArea[4] = {0.01f, 0.21f, MIN_X_COOR - 0.05f, 0.26f};

	const spring_time curTime = spring_now();
	const spring_time maxTime = spring_secs(MAX_FRAMES_HIST_TIME);

	{
		// background
		rb.AddVertex({ {drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor }); // tl
		rb.AddVertex({ {drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[3] + 20.0f * globalRendering->pixelY, 0.0f}, barColor }); // bl
		rb.AddVertex({ {drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[3] + 20.0f * globalRendering->pixelY, 0.0f}, barColor }); // br

		rb.AddVertex({ {drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[3] + 20.0f * globalRendering->pixelY, 0.0f}, barColor }); // br
		rb.AddVertex({ {drawArea[2] + 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor }); // tr
		rb.AddVertex({ {drawArea[0] - 10.0f * globalRendering->pixelX, drawArea[1] - 10.0f * globalRendering->pixelY, 0.0f}, barColor }); // tl
	}

	// title and legend
	font->glFormat(drawArea[0], drawArea[3] + 10 * globalRendering->pixelY, 0.7f, FONT_TOP | DBG_FONT_FLAGS | FONT_BUFFERED,
		"Frame Grapher (%.2fsec)"
		"\xff\xff\x80\xff  GC"
		"\xff\xff\xff\x01  Unsynced"
		"\xff\x01\x01\xff  Swap"
		"\xff\x01\xff\x01  Video"
		"\xff\xff\x01\x01  Sim"
		, MAX_FRAMES_HIST_TIME
	);

	DrawTimeSlices(lgcFrames, maxTime, drawArea, {1.0f, 0.5f, 1.0f, 0.55f}); // gc frames
	DrawTimeSlices(uusFrames, maxTime, drawArea, {1.0f, 1.0f, 0.0f, 0.90f}); // unsynced-update frames
	DrawTimeSlices(swpFrames, maxTime, drawArea, {0.0f, 0.0f, 1.0f, 0.55f}); // video swap frames
	DrawTimeSlices(vidFrames, maxTime, drawArea, {0.0f, 1.0f, 0.0f, 0.55f}); // video frames
	DrawTimeSlices(simFrames, maxTime, drawArea, {1.0f, 0.0f, 0.0f, 0.55f}); // sim frames

	{
		// draw 'feeder' (indicates current time pos)
		const float r = (curTime % maxTime).toSecsf() / MAX_FRAMES_HIST_TIME;
		const float xf = drawArea[0] + r * (drawArea[2] - drawArea[0]);

		rb.AddVertex({{xf                               , drawArea[1], 0.0f}, feederColor}); // tl
		rb.AddVertex({{xf                               , drawArea[3], 0.0f}, feederColor}); // bl
		rb.AddVertex({{xf + 10 * globalRendering->pixelX, drawArea[3], 0.0f}, feederColor}); // br

		rb.AddVertex({{xf + 10 * globalRendering->pixelX, drawArea[3], 0.0f}, feederColor}); // br
		rb.AddVertex({{xf + 10 * globalRendering->pixelX, drawArea[1], 0.0f}, feederColor}); // tr
		rb.AddVertex({{xf                               , drawArea[1], 0.0f}, feederColor}); // tl
	}
	{
		// draw scale (horizontal bar that indicates 30FPS timing length)
		const float xs1 = drawArea[2] - 1.0f / (30.0f * MAX_FRAMES_HIST_TIME) * (drawArea[2] - drawArea[0]);
		const float xs2 = drawArea[2] + 0.0f                                  * (drawArea[2] - drawArea[0]);

		rb.AddVertex({{xs1, drawArea[3] +  2.0f * globalRendering->pixelY, 0.0f}, feederColor}); // tl
		rb.AddVertex({{xs1, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, feederColor}); // bl
		rb.AddVertex({{xs2, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, feederColor}); // br

		rb.AddVertex({{xs2, drawArea[3] + 10.0f * globalRendering->pixelY, 0.0f}, feederColor}); // br
		rb.AddVertex({{xs2, drawArea[3] +  2.0f * globalRendering->pixelY, 0.0f}, feederColor}); // tr
		rb.AddVertex({{xs1, drawArea[3] +  2.0f * globalRendering->pixelY, 0.0f}, feederColor}); // tl
	}
}


static void DrawProfiler(TypedRenderBuffer<VA_TYPE_C   >& rb)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& profiler = CTimeProfiler::GetInstance();
	font->SetTextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// this locks a mutex, so don't call it every frame
	if ((globalRendering->drawFrame % 10) == 0)
		profiler.RefreshProfiles();

	constexpr SColor winColor = SColor{ 0.0f, 0.0f, 0.5f, 0.5f };
	constexpr float textSize = 0.4f;

	const auto& sortedProfiles = profiler.GetSortedProfiles();

	// draw the window background
	{
		rb.AddVertex({{MIN_X_COOR, MIN_Y_COOR - sortedProfiles.size() * LINE_HEIGHT - 0.010f, 0.0f}, winColor}); // tl
		rb.AddVertex({{MIN_X_COOR, MIN_Y_COOR +                         LINE_HEIGHT + 0.005f, 0.0f}, winColor}); // bl
		rb.AddVertex({{MAX_X_COOR, MIN_Y_COOR +                         LINE_HEIGHT + 0.005f, 0.0f}, winColor}); // br

		rb.AddVertex({{MAX_X_COOR, MIN_Y_COOR +                         LINE_HEIGHT + 0.005f, 0.0f}, winColor}); // br
		rb.AddVertex({{MAX_X_COOR, MIN_Y_COOR - sortedProfiles.size() * LINE_HEIGHT - 0.010f, 0.0f}, winColor}); // tr
		rb.AddVertex({{MIN_X_COOR, MIN_Y_COOR - sortedProfiles.size() * LINE_HEIGHT - 0.010f, 0.0f}, winColor}); // tl
	}

	// table header
	{
		const float fStartY = MIN_Y_COOR + 0.005f;
		      float fStartX = MIN_X_COOR + 0.005f + 0.015f + 0.005f;

		// print total-time running since application start
		font->glPrint(fStartX += 0.04f, fStartY, textSize, FONT_SHADOW | FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "sum-time");

		// print percent of CPU time used within the last 500ms
		font->glPrint(fStartX += 0.06f, fStartY, textSize, FONT_SHADOW | FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "cur-%usage");
		font->glPrint(fStartX += 0.04f, fStartY, textSize, FONT_SHADOW | FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "max-%usage");
		font->glPrint(fStartX += 0.04f, fStartY, textSize, FONT_SHADOW | FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "lag");

		// print timer name
		font->glPrint(fStartX += 0.01f, fStartY, textSize, FONT_SHADOW | FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_BUFFERED, "title");
	}

	// draw the textual info (total-time, short-time percentual time, timer-name)
	int y = 1;

	for (const auto& p: sortedProfiles) {
		const auto& profileData = p.second;

		const float fStartY = MIN_Y_COOR - (y++) * LINE_HEIGHT;
		      float fStartX = MIN_X_COOR + 0.005f + 0.015f + 0.005f;

		// print total-time running since application start
		font->glFormat(fStartX += 0.04f, fStartY, textSize, FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "%.2fs", profileData.total.toSecsf());

		// print percent of CPU time used within the last 500ms
		font->glFormat(fStartX += 0.06f, fStartY, textSize, FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "%.2f%%", profileData.stats.y * 100.0f);
		font->glFormat(fStartX += 0.04f, fStartY, textSize, FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "\xff\xff%c%c%.2f%%", profileData.newPeak? 1: 255, profileData.newPeak? 1: 255, profileData.stats.z * 100.0f);
		font->glFormat(fStartX += 0.04f, fStartY, textSize, FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_RIGHT | FONT_BUFFERED, "\xff\xff%c%c%.0fms", profileData.newLagPeak? 1: 255, profileData.newLagPeak? 1: 255, profileData.stats.x);

		// print timer name
		font->glPrint(fStartX += 0.01f, fStartY, textSize, FONT_DESCENDER | FONT_SCALE | FONT_NORM | FONT_BUFFERED, p.first);
	}


	{
		// draw the Timer boxes
		const float boxSize = LINE_HEIGHT * 0.9f;
		const float selOffset = boxSize * 0.2f;

		// translate to upper-left corner of first box
		const CMatrix44f boxMat(float3{MIN_X_COOR + 0.005f, MIN_Y_COOR + boxSize, 0.0f});

		int i = 1;

		for (const auto& p: sortedProfiles) {
			const CTimeProfiler::TimeRecord& tr = p.second;

			const SColor selColor(tr.color.x, tr.color.y, tr.color.z, 1.0f);
			const SColor actColor(1.0f, 0.0f, 0.0f, 1.0f);

			// selection box
			rb.AddVertex({boxMat * float3(   0.0f, -i * LINE_HEIGHT          , 0.0f), selColor}); // tl
			rb.AddVertex({boxMat * float3(   0.0f, -i * LINE_HEIGHT - boxSize, 0.0f), selColor}); // bl
			rb.AddVertex({boxMat * float3(boxSize, -i * LINE_HEIGHT - boxSize, 0.0f), selColor}); // br

			rb.AddVertex({boxMat * float3(boxSize, -i * LINE_HEIGHT - boxSize, 0.0f), selColor}); // br
			rb.AddVertex({boxMat * float3(boxSize, -i * LINE_HEIGHT          , 0.0f), selColor}); // tr
			rb.AddVertex({boxMat * float3(   0.0f, -i * LINE_HEIGHT          , 0.0f), selColor}); // tl

			// activated box
			if (tr.showGraph) {
				rb.AddVertex({boxMat * float3(LINE_HEIGHT +           selOffset, -i * LINE_HEIGHT -           selOffset, 0.0f), actColor}); // tl
				rb.AddVertex({boxMat * float3(LINE_HEIGHT +           selOffset, -i * LINE_HEIGHT - boxSize + selOffset, 0.0f), actColor}); // bl
				rb.AddVertex({boxMat * float3(LINE_HEIGHT + boxSize - selOffset, -i * LINE_HEIGHT - boxSize + selOffset, 0.0f), actColor}); // br

				rb.AddVertex({boxMat * float3(LINE_HEIGHT + boxSize - selOffset, -i * LINE_HEIGHT - boxSize + selOffset, 0.0f), actColor}); // br
				rb.AddVertex({boxMat * float3(LINE_HEIGHT + boxSize - selOffset, -i * LINE_HEIGHT -           selOffset, 0.0f), actColor}); // tr
				rb.AddVertex({boxMat * float3(LINE_HEIGHT +           selOffset, -i * LINE_HEIGHT -           selOffset, 0.0f), actColor}); // tl
			}

			i++;
		}
	}

	// flush all quad elements
	rb.Submit(GL_TRIANGLES);

	// draw the graph lines
	//GL::WideLineAdapterC* wla = GL::GetWideLineAdapterC();
	//wla->Setup(buffer, globalRendering->viewSizeX, globalRendering->viewSizeY, 3.0f, CMatrix44f::ClipOrthoProj01());
	glLineWidth(3.0f);

	for (const auto& p: sortedProfiles) {
		const CTimeProfiler::TimeRecord& tr = p.second;

		const float3& fc = tr.color;
		const SColor c(fc[0], fc[1], fc[2]);

		if (!tr.showGraph)
			continue;

		const float range_x = MAX_X_COOR - MIN_X_COOR;
		const float steps_x = range_x / CTimeProfiler::TimeRecord::numFrames;

		for (size_t a = 0; a < CTimeProfiler::TimeRecord::numFrames; ++a) {
			// profile runtime; 0.5f means 50% of a CPU was used during that frame
			// this may exceed 1.0f if an operation which ran over multiple frames
			// ended in this one
			const float p = tr.frames[a].toSecsf() * GAME_SPEED;
			const float x = MIN_X_COOR + (a * steps_x);
			const float y = 0.02f + (p * 0.96f);

			rb.AddVertex({{x, y, 0.0f}, c});
		}

		rb.Submit(GL_LINE_STRIP);
	}

	glLineWidth(1.0f);
}


static void DrawInfoText(TypedRenderBuffer<VA_TYPE_C   >& rb)
{
	RECOIL_DETAILED_TRACY_ZONE;
	constexpr SColor bgColor = SColor{0.0f, 0.0f, 0.0f, 0.5f};

	// background

	rb.AddVertex({{             0.01f - 10.0f * globalRendering->pixelX, 0.02f - 10.0f * globalRendering->pixelY, 0.0f}, bgColor}); // tl
	rb.AddVertex({{             0.01f - 10.0f * globalRendering->pixelX, 0.17f + 20.0f * globalRendering->pixelY, 0.0f}, bgColor}); // bl
	rb.AddVertex({{MIN_X_COOR - 0.05f + 10.0f * globalRendering->pixelX, 0.17f + 20.0f * globalRendering->pixelY, 0.0f}, bgColor}); // br

	rb.AddVertex({{MIN_X_COOR - 0.05f + 10.0f * globalRendering->pixelX, 0.17f + 20.0f * globalRendering->pixelY, 0.0f}, bgColor}); // br
	rb.AddVertex({{MIN_X_COOR - 0.05f + 10.0f * globalRendering->pixelX, 0.02f - 10.0f * globalRendering->pixelY, 0.0f}, bgColor}); // tr
	rb.AddVertex({{             0.01f - 10.0f * globalRendering->pixelX, 0.02f - 10.0f * globalRendering->pixelY, 0.0f}, bgColor}); // tl


	// print performance-related information (timings, particle-counts, etc)
	font->SetTextColor(1.0f, 1.0f, 0.5f, 0.8f);

	constexpr const char* fpsFmtStr = "[1] {Draw,Sim}FrameRate={%0.1f, %0.1f}Hz";
	constexpr const char* ctrFmtStr = "[2] {Draw,Sim}FrameTick={%u, %d}";
	constexpr const char* avgFmtStr = "[3] {Sim,Update,Draw}FrameTime={%s%2.1f, %s%2.1f, %s%2.1f (GL=%2.1f)}ms";
	constexpr const char* spdFmtStr = "[4] {Current,Wanted}SimSpeedMul={%2.2f, %2.2f}x";
	constexpr const char* sfxFmtStr = "[5] {Synced,Unsynced}Projectiles={%u,%u} Particles=%u Saturation=%.1f";
	constexpr const char* pfsFmtStr = "[6] (%s)PFS-updates queued: {%i, %i}";
	constexpr const char* luaFmtStr = "[7] Lua-allocated memory: %.1fMB (%.1fK allocs : %.5u usecs : %.1u states)";
	constexpr const char* gpuFmtStr = "[8] GPU-allocated memory: %.1fMB / %.1fMB";
	constexpr const char* sopFmtStr = "[9] SOP-allocated memory: {U,F,P,W}={%.1f/%.1f, %.1f/%.1f, %.1f/%.1f, %.1f/%.1f}KB";

	const CProjectileHandler* ph = &projectileHandler;
	const IPathManager* pm = pathManager;

	font->glFormat(0.01f, 0.02f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, fpsFmtStr, globalRendering->FPS, gu->simFPS);
	font->glFormat(0.01f, 0.04f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, ctrFmtStr, globalRendering->drawFrame, gs->frameNum);

	// 16ms := 60fps := 30simFPS + 30drawFPS
	font->glFormat(0.01f, 0.06f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, avgFmtStr,
		(gu->avgSimFrameTime  > 16) ? "\xff\xff\x01\x01" : "", gu->avgSimFrameTime,
		(gu->avgFrameTime     > 30) ? "\xff\xff\x01\x01" : "", gu->avgFrameTime,
		(gu->avgDrawFrameTime > 16) ? "\xff\xff\x01\x01" : "", gu->avgDrawFrameTime,
		(globalRendering->CalcGLDeltaTime(CGlobalRendering::FRAME_REF_TIME_QUERY_IDX, CGlobalRendering::FRAME_END_TIME_QUERY_IDX) * 0.001f) * 0.001f
	);

	font->glFormat(0.01f, 0.08f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, spdFmtStr, gs->speedFactor, gs->wantedSpeedFactor);
	font->glFormat(0.01f, 0.10f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, sfxFmtStr, ph->GetActiveProjectiles(true).size(), ph->GetActiveProjectiles(false).size(), ph->GetCurrentParticles(), ph->GetParticleSaturation(true));

	{
		const int2 pfsUpdates = pm->GetNumQueuedUpdates();

		switch (pm->GetPathFinderType()) {
			case NOPFS_TYPE: {
				font->glFormat(0.01f, 0.12f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, pfsFmtStr, "NO", pfsUpdates.x, pfsUpdates.y);
			} break;
			case HAPFS_TYPE: {
				font->glFormat(0.01f, 0.12f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, pfsFmtStr, "HA", pfsUpdates.x, pfsUpdates.y);
			} break;
			case QTPFS_TYPE: {
				font->glFormat(0.01f, 0.12f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, pfsFmtStr, "QT", pfsUpdates.x, pfsUpdates.y);
			} break;
			default: {
			} break;
		}
	}

	{
		SLuaAllocState state = {{0}, {0}, {0}, {0}};
		spring_lua_alloc_get_stats(&state);

		const    float allocMegs = state.allocedBytes.load() / 1024.0f / 1024.0f;
		const    float kiloAlloc = state.numLuaAllocs.load() / 1000.0f;
		const uint32_t allocTime = state.luaAllocTime.load();
		const uint32_t numStates = state.numLuaStates.load();

		font->glFormat(0.01f, 0.14f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, luaFmtStr, allocMegs, kiloAlloc, allocTime, numStates);
	}

	{
		int2 vidMemInfo;

		GetAvailableVideoRAM(&vidMemInfo.x, globalRenderingInfo.glVendor);

		font->glFormat(0.01f, 0.16f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, gpuFmtStr, (vidMemInfo.x - vidMemInfo.y) / 1024.0f, vidMemInfo.x / 1024.0f);
	}

	font->glFormat(0.01f, 0.18f, 0.5f, DBG_FONT_FLAGS | FONT_BUFFERED, sopFmtStr,
		unitMemPool.alloc_size() / 1024.0f,
		unitMemPool.freed_size() / 1024.0f,
		featureMemPool.alloc_size() / 1024.0f,
		featureMemPool.freed_size() / 1024.0f,
		projMemPool.alloc_size() / 1024.0f,
		projMemPool.freed_size() / 1024.0f,
		weaponMemPool.alloc_size() / 1024.0f,
		weaponMemPool.freed_size() / 1024.0f
	);
}



void ProfileDrawer::DrawScreen()
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& rb = RenderBuffer::GetTypedRenderBuffer<VA_TYPE_C>();
	auto& shader = rb.GetShader();

	glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
	glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadMatrixf(CMatrix44f::ClipOrthoProj01());

	shader.Enable();

	font->SetTextColor(1.0f, 1.0f, 0.5f, 0.8f);

	DrawThreadBarcode(rb);
	DrawFrameBarcode(rb);

	// text before profiler to batch the background
	DrawInfoText(rb);
	DrawProfiler(rb);
	DrawBufferStats({0.01f, 0.605f});

	shader.Disable();

	font->DrawBuffered();

	glMatrixMode(GL_PROJECTION); glPopMatrix();
	glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

bool ProfileDrawer::MousePress(int x, int y, int button)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& profiler = CTimeProfiler::GetInstance();
	if (!IsAbove(x, y))
		return false;

	const float my = CInputReceiver::MouseY(y);
	const int selIndex = int((MIN_Y_COOR - my) / LINE_HEIGHT);

	if (selIndex < 0)
		return false;
	if (selIndex >= profiler.GetNumSortedProfiles())
		return false;

	auto& sortedProfiles = profiler.GetSortedProfiles();
	auto& timeRecord = sortedProfiles[selIndex].second;

	// switch the selected Timers showGraph value
	// this reverts when the profile is re-sorted
	timeRecord.showGraph = !timeRecord.showGraph;
	return true;
}

bool ProfileDrawer::IsAbove(int x, int y)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const float mx = CInputReceiver::MouseX(x);
	const float my = CInputReceiver::MouseY(y);

	// check if a Timer selection box was hit
	return (mx >= MIN_X_COOR && mx <= MAX_X_COOR && my >= (MIN_Y_COOR - CTimeProfiler::GetInstance().GetNumSortedProfiles() * LINE_HEIGHT) && my <= MIN_Y_COOR);
}


void ProfileDrawer::DbgTimingInfo(DbgTimingInfoType type, const spring_time start, const spring_time end)
{
	RECOIL_DETAILED_TRACY_ZONE;
	if (!IsEnabled())
		return;

	switch (type) {
		case TIMING_VIDEO: {
			vidFrames.emplace_back(start, end);
		} break;
		case TIMING_SIM: {
			simFrames.emplace_back(start, end);
		} break;
		case TIMING_GC: {
			lgcFrames.emplace_back(start, end);
		} break;
		case TIMING_SWAP: {
			swpFrames.emplace_back(start, end);
		} break;
		case TIMING_UNSYNCED: {
			uusFrames.emplace_back(start, end);
		} break;
		default: {
		} break;
	}
}

static void DiscardOldTimeSlices(
       std::deque<TimeSlice>& frames,
       const spring_time curTime,
       const spring_time maxTime
) {
	RECOIL_DETAILED_TRACY_ZONE;
	// remove old entries
	while (!frames.empty() && (curTime - frames.front().second) > maxTime) {
		frames.pop_front();
	}
}

void ProfileDrawer::Update()
{
	RECOIL_DETAILED_TRACY_ZONE;
	const spring_time curTime = spring_now();
	const spring_time maxTime = spring_secs(MAX_FRAMES_HIST_TIME);

	// cleanup old frame records
	DiscardOldTimeSlices(lgcFrames, curTime, maxTime);
	DiscardOldTimeSlices(uusFrames, curTime, maxTime);
	DiscardOldTimeSlices(swpFrames, curTime, maxTime);
	DiscardOldTimeSlices(vidFrames, curTime, maxTime);
	DiscardOldTimeSlices(simFrames, curTime, maxTime);

	// old ThreadProfile records get cleaned up inside TimeProfiler and DrawThreadBarcode
}

