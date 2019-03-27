#include<thread>
#include<Windows.h>


using namespace std;


#include "cinder/app/App.h"
#include "cinder/app/RendererGl.h"
#include "cinder/app/Window.h"
#include "cinder/gl/gl.h"

using namespace ci;
using namespace ci::app;

#include "zlib/zlib.h"


//Preprocessor flags
//	Directive			Result
//	data_collect_mode	Disables console output (only where it can cause delays, distorting data) and writes execution statistics to a file at the end of the sort
//	auto_size			Automatically sets the size of the window

#define data_collect_mode
#define auto_size
//#define allow_manual_quit

typedef std::vector<unsigned> t_dataset;

using std::cout;
using std::cin;
using std::endl;

#ifndef __linux__
#include "zlib/general.h"
#include "zlib/var.h"
#else
#include "general.h"
#include "varTypes.h"
#include "varConv.h"
#endif

const unsigned data_length = std::pow(10, 5);		//2 E 5 is pretty much the upper limit for the number of values that can be sorted in a reasonable amount of time.
const unsigned max_threads = 8;
const unsigned min_threads = 1;

const unsigned dataGen_maxValue = 10000;

#ifdef auto_size
const unsigned wind_width = 920;
const unsigned wind_height = 828;
#endif

unsigned * knownDataset;

zlib::timer zClock;

std::vector<std::thread> threads;	//Threads spawned by sortDemo::multiSort::multiSort().  Accessable here so they can be nuked during program termination.

struct timePair		//A pair of times storing the time a task started and the time it ended
{
	//The data itself
	double _begin;
	double _end;

	//Store the current time in each datapoint
	void begin() { _begin = zClock.getTime(); }
	void end() { _end = zClock.getTime(); }

	void reset()	//Reset values to -1
	{
		_begin = -1;
		_end = -1;
	}

	string toString()
	{
		return conv::toString(_begin) + " to " + conv::toString(_end);
	}

	double duration()
	{
		if(_begin == -1) return 0;	//If the clock never started, the duration is 0
		if(_end == -1) return zClock.getTime() - _begin;	//If the timePair has no end, use right now as it's "end"
		return _end - _begin;
	}
};

struct runtimeStats	//The execution stats for an entire sort job
{
	zlib::var::longTime launch;					//When the job was started

	unsigned _num_threads;						//Number of threads to be used
	unsigned _datasetSize;						//#elements in the set

	timePair minMax;							//Identifying the min/max values for the set

	timePair * secondaryChunks;					//Generating secondary chunks

	timePair secondaryChunks_threadTasking;		//Tasking threads that generate secondary chunks
	timePair secondaryChunks_threadClosing;		//Joining threads that generate secondary chunks
	timePair secondaryChunks_merging;			//Merging secondary chunks from each thread together into big chunks


	timePair * sorting;							//Sorting the chunks

	timePair sorting_threadTasking;				//Tasking threads that sort the chunks
	timePair sorting_threadClosing;				//Joining threads that sort the chunks
	timePair sorting_compilingOutput;			//Combine the output from each sorted chunk together into a sorted dataset

	void init(unsigned __nThreads, unsigned __dat_length)	//Clears out values, allocates arrays based on num_threads, initialize values
	{
		launch = zlib::var::longTime::now();

		_num_threads = __nThreads;
		_datasetSize = __dat_length;

		minMax.reset();
		secondaryChunks_threadTasking.reset();
		secondaryChunks_threadClosing.reset();
		secondaryChunks_merging.reset();

		sorting_threadTasking.reset();
		sorting_threadClosing.reset();
		sorting_compilingOutput.reset();

		//I think we may have a bit of a memory leak here?  We may need to deallocate the old arrays explicitly.
		secondaryChunks = new timePair[_num_threads];
		sorting = new timePair[_num_threads];
	}

	void toFile(string filename = "multiSort_output")
	{
		string timestamp = launch.getYMD() + "_" + launch.getHMS();
		string filepath = filename + "_" + timestamp + ".txt";
#ifdef _WIN32
		filepath = "../" + filepath;	//If we're on windows, 'step' up a directory for our output
#endif

		/*std::ofstream out;
		out.open(filepath);

		out << "Launched at: " << timestamp << endl;
		out << "Number of threads: " << _num_threads << endl;
		out << "Dataset size: " << _datasetSize << endl;

		out << "Finding min/max: " << minMax.toString() << endl;

		out << "Secondary Thread Stats" << endl;
		for(unsigned i = 0; i < _num_threads; i++)
		{
			out << "\tThread " << i << ": " << secondaryChunks[i].toString() << endl;
		}

		out << "\tThread tasking: " << secondaryChunks_threadTasking.toString() << endl;
		out << "\tThread closing: " << secondaryChunks_threadClosing.toString() << endl;
		out << "\tData merging:   " << secondaryChunks_merging.toString() << endl;

		out << endl;

		out << "Sort Stats:" << endl;
		for(unsigned i = 0; i < _num_threads; i++)
		{
			out << "\tThread " << i << ": " << sorting[i].toString() << endl;
		}

		out << "\tThread tasking: " << sorting_threadTasking.toString() << endl;
		out << "\tThread closing: " << sorting_threadClosing.toString() << endl;
		out << "\tData compilation: " << sorting_compilingOutput.toString() << endl;

		out.close();*/
	}
};

using zlib::timer;

namespace data
{
	string sortStatus;

	bool _dataFrozen = false;

	runtimeStats * current;
	runtimeStats current_cache;	//A cached version of the current stats

	runtimeStats stats_8thr;
	runtimeStats stats_4thr;
	runtimeStats stats_2thr;
	runtimeStats stats_1thr;

	timePair threadStats[8];

	unsigned currThreadBatchExecuting = -1;

	unsigned nThreads;		

	void freezeData() {
		_dataFrozen = true;
		current_cache = *current;
	}
	void unfreezeData() { _dataFrozen = false; }

	runtimeStats getData() { 
		return (_dataFrozen || current == NULL) ? current_cache : *current; 
	}
}

namespace sortDemo
{
	runtimeStats * runStats;

	unsigned * dataset = new unsigned[data_length];

	struct t_secondary_chunks
	{
		t_secondary_chunks() {}
		t_secondary_chunks(unsigned num) { chunks = new t_dataset[num]; }

		t_dataset * chunks;
	};


	const bool validatedataset(t_dataset const & data)
	{
		for(unsigned i = 0; i < data.size() - 1; i++)
		{
			if(data[i] > data[i + 1]) return false;
		}
		return true;
	}

	//Returns -1 if valid, > 0 if otherwise.  (it will be the index in the data that is error'd)
	const int validatedataset(unsigned * data)
	{
		for(unsigned i = 0; i < data_length ; i++)
		{
			if(data[i] > data[i + 1]) return i;
		}
		return -1;
	}


	void genRand(unsigned * data, unsigned length = data_length)
	{
		srand(time(NULL));
		for(unsigned i = 0; i < length; i++)
		{
			data[i] = (rand() % dataGen_maxValue);
		}
	}

	void resetDataset(unsigned * data)
	{
		for(unsigned i = 0; i < data_length; i++)
		{
			data[i] = knownDataset[i];
		}
		Sleep(500);
	}

	namespace multiSort
	{

		unsigned num_threads;	//The number of threads to be used to process the data
		unsigned chunk_size;	//The size of each chunk being processed

								//The maximum and minimum values in the data
		unsigned data_max;
		unsigned data_min;

		runtimeStats stats;

		void findMinMax(unsigned * data, unsigned & out_min, unsigned & out_max, unsigned len = data_length)
		{
#ifndef data_collect_mode
			cout << "Identifying min/max values for the dataset..." << endl;
#endif
			timer begin;

			//"seed" the min & max
			data_min = data[0];
			data_max = data[0];

			for(unsigned i = 0; i < len; i++)
			{
				if(data[i] > out_max) out_max = data[i];
				if(data[i] < out_min) out_min = data[i];
			}

#ifndef data_collect_mode
			cout << "Values identified in " << begin.getTime() << " seconds." << endl;
			cout << "Min: " << data_min << "  Max: " << data_max << endl;
#endif
		}

		void sort_secondaryChunks(unsigned * data, unsigned start, unsigned end, t_secondary_chunks & output, timePair * stats)
		{
			stats->begin();
			unsigned value_range = (data_max - data_min) / num_threads;		//The "width" of values covered in each secondary chunk 

			auto getRange = [](unsigned index)	//Returns the beginning of the range assigned to the chunk for the given index
			{
				//All the casting and rounding is here to make sure data loss doesn't end up producing chunks that exclude the highest value
				return (unsigned)std::ceil((double)index * (double)((double)(data_max - data_min) / num_threads)) + data_min;	//We use data_max - data_min + 1 so that the highest value isn't written to the 20th chunk, which does not exist
			};

			//So I think the issue here is the 'new' allocating space for the array changes the location at which the 'head' pointer is stored.

			//Loop over every position in the data
			for(unsigned dataPos = start; dataPos < end; dataPos++)
			{
				//Inline variables would be nice here, just gotta wait cor C++17....
				//Calculates which secondary chunk the value should be within
				unsigned index = (num_threads * (data[dataPos] - data_min)) / (data_max - data_min + 1);

				output.chunks[index].push_back(data[dataPos]);			//Add the value to the secondary chunk determined
			}
			stats->end();
		}

		//Ideally we'd use a reference here but it appears threads don't track references.  (well, std::bind doesnt?  I think)
		/*void bubbleSort(t_dataset * data, timePair * stats)
		{
			stats->begin();
			bool changed = true;
			while(changed)
			{
				changed = false;
				for(unsigned i = 0; i < (*data).size() - 1; i++)
				{
					if((*data)[i] >(*data)[i + 1])
					{
						std::swap((*data)[i], (*data)[i + 1]);
						changed = true;
					}
				}
			}
			stats->end();

			//bool status = validatedataset((*data));
#ifndef data_collect_mode
			cout << "";
#endif
		}*/

		void bubbleSort(unsigned * data, timePair * stats, unsigned iter_start, unsigned iter_end)
		{
			
			bool changed = true;	//Whether or not the sort has made any changes to value orders
			while(changed)
			{
				changed = false;
				//Loop over the dataset we're working on
				for(unsigned i = iter_start; i < iter_end - 1; i++)
				{
					//If the data at 'i' is greater than the data at 'i + 1' swap them
					if(data[i] > data[i + 1])
					{
						std::swap(data[i], data[i + 1]);
						changed = true;
					}
				}
			}
			
		}

		void shakerSort(unsigned * data, timePair * stats, unsigned iter_start, unsigned iter_end)
		{
			bool changed = true;
			while(changed)
			{
				changed = false;

				for(unsigned i = iter_start; i < iter_end - 1; i ++)
				{
					if(data[i] > data[i + 1])
					{
						std::swap(data[i], data[i + 1]);
						changed = true;
					}
				}

				for(unsigned i = iter_end - 2; i > iter_start; i--)
				{
					if(data[i] > data[i + 1])
					{
						std::swap(data[i], data[i + 1]);
						changed = true;
					}
				}

			}

		}

		void sortWrapper(unsigned * data, timePair * stats, unsigned iter_start, unsigned iter_end, unsigned thread_num)
		{
			data::threadStats[thread_num].begin();
			stats->begin();	//Start timing the sort

			shakerSort(data, stats, iter_start, iter_end);

			stats->end();	//Stop timing
			data::threadStats[thread_num].end();
		}

		void multiSort(unsigned * dataset, unsigned num_threads_to_use)	//Sorts 'data' from least to greatest, and stores it in 'output'.  'data' is expected to be of length 'data_length, and 'output' will be allocated within the function
		{

			data::current = &stats;		//Set the 'current stats' pointer to direct to the current stats 'file'
			zClock = zlib::timer();	//Reset the timer so that time can be tracked with the beginning of execution = 0

									//Update some 'runtime constants' (values that don't change for each 'instance' of the sort, but can be changed between sorts)
			num_threads = num_threads_to_use;
			chunk_size = data_length / num_threads;

			stats.init(num_threads, data_length);		//Set up our statistic logging

			for(unsigned i = 0; i < 8; i++)
			{
				data::threadStats[i].reset();
			}

			stats.minMax.begin();

			//data::sortStatus = "finding minMax";
			//Identify the minimum and maximum values in the dataset
			findMinMax(dataset, data_min, data_max);

			stats.minMax.end();

			//The primary chunks aren't copied from the dataset, instead the threads are just given the start and endpoints of the chunk within the dataset
			
			//The secondary chunks, indexed via [thread][secondary chunk] which gives the dataset, can be further indexed to get each individual elment in the set.  The base layer is a vector bc we don't know how large the dataset will be
			//t_dataset **  chunks_secondary = new t_dataset* [num_threads];

			t_secondary_chunks * chunks_secondary = new t_secondary_chunks[num_threads];

			//Initialize the vectors within the list
			for(unsigned i = 0; i < num_threads; i++)
			{
				chunks_secondary[i] = t_secondary_chunks(num_threads);
			}


			auto indexPos = [](unsigned i)	//Converts an index into the corresponding position in the dataset
			{
				return (unsigned)i * (data_length / (unsigned)num_threads) + (unsigned)data_min;		//Probably overdid it on the casting, but I wanted to make sure it wouldn't get converted to a smaller type.
			};

			stats.secondaryChunks_threadTasking.begin();
			//data::sortStatus = "Chunking";
			//Spawn threads to sort the dataset into secondary chunks 
			for(unsigned i = 0; i < num_threads; i++)
			{
				threads.push_back(
					std::thread{std::bind(
						sort_secondaryChunks, dataset, indexPos(i), indexPos(i + 1), chunks_secondary[i], &(stats.secondaryChunks[i])
					)}
				);
			}

			stats.secondaryChunks_threadTasking.end();
			stats.secondaryChunks_threadClosing.begin();

			//Wait for the threads to finish sorting
			for(unsigned i = 0; i < threads.size(); i++) threads[i].join();

			threads.clear();	//Clear out the threads vector since they've all resolved

			stats.secondaryChunks_threadClosing.end();


			//Combining equivalent secondary chunks
			stats.secondaryChunks_merging.begin();

			//data::sortStatus = "merging secondary chunks";
			
			//The position that each chunk starts at (look ahead by one for the position that the chunk ends at + 1)
			unsigned * chunkHeads = new unsigned[num_threads + 1];
			chunkHeads[num_threads] = data_length + 1;	//Mark the last position in the dataset

			{//Merge all the various secondary chunks together
				unsigned pos = 0;	//The next position in the dataset to be written to

				for(unsigned iter_chunk = 0; iter_chunk < num_threads; iter_chunk++)
				{
					chunkHeads[iter_chunk] = pos;
					for(unsigned iter_thread = 0; iter_thread < num_threads; iter_thread++)
					{
						t_dataset * curr = &(chunks_secondary[iter_thread].chunks[iter_chunk]);
						for(auto iter = curr->begin(); iter != curr->end(); iter++)
						{
							dataset[pos] = *iter;
							pos++;
						}
					}
				}
			}			
			
			stats.secondaryChunks_merging.end();

			//Task threads to sort the new chunks
			//data::sortStatus = "Tasking sorting threads";
			stats.sorting_threadTasking.begin();
			for(unsigned i = 0; i < num_threads; i++)
			{
				threads.push_back(std::thread{std::bind(
					sortWrapper, dataset, &(stats.sorting[i]), chunkHeads[i], chunkHeads[i+1], i
				)});
			}
			stats.sorting_threadTasking.end();

			//data::sortStatus = "Sorting";
			stats.sorting_threadClosing.begin();
			for(unsigned i = 0; i < threads.size(); i++)
			{
				threads[i].join();
			}

			threads.clear();
			stats.sorting_threadClosing.end();
			
			/*
			//Complile individual sets into output dataset
			//Once the chunks are dumped back into the array this won't be necessary

			data::sortStatus = "Merging Output";
			stats.sorting_compilingOutput.begin();
			output.reserve(data_length);

			for(unsigned i = 0; i < num_threads; i++)
			{
				output.insert(output.end(), combined_chunks[i].begin(), combined_chunks[i].end());
			}
			stats.sorting_compilingOutput.end();*/

			int valid = validatedataset(dataset);

			if(valid != -1)
			{
				app::console() << "@"<< valid << endl;
				for(unsigned i = valid - 5; i < valid + 5; i++)
				{
					app::console() << dataset[i] << endl;
				}

				//_DEBUG_ERROR("Dataset sort incomplete!");
			}
		}
	}

	void runSortFor(unsigned nThreads)
	{
		data::currThreadBatchExecuting = nThreads;
		data::sortStatus = "Generating Dataset";
		resetDataset(dataset);
		data::sortStatus = "Sorting - " + conv::toString(nThreads) + " threads";
		multiSort::multiSort(dataset, nThreads);
		//Sit tight for a bit so you can see the data sorted
		Sleep(2500);
	}

	void runDemo()
	{
		runStats = new runtimeStats[max_threads];

		data::sortStatus = "Launching Batch Jobs";
		
		while(true)
		{
			runSortFor(8);
			data::stats_8thr = multiSort::stats;
			runSortFor(4);
			data::stats_4thr = multiSort::stats;
			runSortFor(2);
			data::stats_2thr = multiSort::stats;
			//runSortFor(1);
			//data::stats_1thr = multiSort::stats;
		}

		data::sortStatus = "All jobs complete";


		//Write compiled thread stats to .csv
		{
			var::longTime firstRun = runStats[max_threads - min_threads].launch;

			string timestamp = firstRun.getYMD() + "_" + firstRun.getHMS();
			string filepath = "multiSort_compiled_" + timestamp + ".csv";

#ifdef _WIN32
			filepath = "../" + filepath;
#endif

			ofstream out;
			out.open(filepath);

			//Write the header
			out << "Num. Threads,";
			for(unsigned i = 0; i < max_threads; i++)
			{
				out << "Thread " << i << " exec,";
			}
			out << endl;

			//Output the data for each thread
			for(unsigned i = 0; i <= max_threads - min_threads; i++)
			{
				unsigned threads = runStats[i]._num_threads;
				//Write the number of threads
				out << threads << ',';

				//Write the execution time for each thread
				for(unsigned j = 0; j < threads; j++)
				{
				//	if(runStats != NULL) out << runStats[i].sorting[j].duration() << ',';
				}
				out << endl;
			}

			out.close();
		}
	}

}




class multiSort_GraphicalApp : public App {
  public:
	void setup() override;
	void mouseDown( MouseEvent event ) override;
	void keyDown(KeyEvent event) override;
	void update() override;
	void draw() override;

	std::thread sortController;
};

void multiSort_GraphicalApp::setup()
{
	WindowRef current = getWindow();

	current->setTitle("CU Open House MultiSort Demo");
#ifdef auto_size
	current->setSize(wind_width, wind_height);
#endif

	//current->setFullScreen(true);

	//This line should position the window on the left edge of the second monitor (theoretically)
	//current->setPos(1920 + (1600 - wind_width), 30);
	//current->setPos(-wind_width-1, 120);

	//Set the window to be non-resizeable
	

	data::sortStatus = "";

	gl::enableAlphaBlending();

	data::current_cache._num_threads = -1;

	knownDataset = new unsigned[data_length];
	sortDemo::genRand(knownDataset);

	sortController = std::thread(sortDemo::runDemo);
}

void multiSort_GraphicalApp::mouseDown( MouseEvent event )
{
}

void multiSort_GraphicalApp::keyDown(KeyEvent event)
{
#ifdef allow_manual_quit
	{
		if(event.getChar() == 'q')
		{
			//Terminate worker threads
			for(unsigned i = 0; i < threads.size(); i++)
			{
				try
				{
					threads[i].~thread();
				}
				catch(...) {}
			}
			try
			{
				sortController.~thread();
			}
			catch(...) {}
			exit(0);
		}
	}
#endif
}

void multiSort_GraphicalApp::update()
{
	window::update();
	//app::console() << window::unscale(var::coord2(getWindowWidth(), 0)).toString() << endl;
	
}

void drawValue(unsigned x, unsigned value, unsigned multiplier)
{
	if(value == NULL || !(0 <= value && value < dataGen_maxValue)) return;
	double height = 15 * ((double)value / (double)dataGen_maxValue);
	unsigned scaledHeight = window::scaleY(height);
	for(unsigned i = 0; i <= multiplier; i++)
	{
		
		gl::drawLine(glm::vec2(x + (multiplier - 1), getWindowHeight()), glm::vec2(x + (multiplier - 1), scaledHeight));
	}
}

void chartDataset(unsigned * dataset, unsigned data_len = data_length)
{
	{	//Data visualizer

		unsigned multiplier = 1;

		gl::color(Color(1, 1, 1));
		unsigned sampleInterval = floor(data_len / getWindowWidth()) * multiplier;

		for(unsigned i = 0; i * multiplier < getWindowWidth(); i++)
		{
			drawValue(i*multiplier, dataset[i * sampleInterval], multiplier);
		}
	}
}

const unsigned FONT_SIZE = 50;


unsigned timeline_length = 3 * 60;	//In seconds

double timeScale = (double)100 / timeline_length;	//Screen Units per Second



void drawThreadProgressBars(runtimeStats * source, unsigned sect_top, unsigned sect_bot, var::color_RGB color = var::color_RGB::BLUE())
{
	//Draw the sort thread progess bars
	{
		vector<double>runtimes;

		//Get the running times for each of the current threads and draw them to the 
		for(unsigned i = 0; i < source->_num_threads; i++)
		{
			if(source->sorting == NULL) break;

			double time;
			if(source->sorting[i]._end < 0)	//If the thread is still executing
			{	//Use the current time
				time = zClock.getTime() - source->sorting[i]._begin;
			}
			else
			{
				//Use the time it stopped
				time = source->sorting[i].duration();
			}

			double xSize = (double)100 * time / timeScale;

			double thread_box_ySize = float(sect_top - sect_bot) / source->_num_threads;	//Get the y-size of each thread drawn

			const double buffer_size = .1 * thread_box_ySize;

			//This part handles the gradient
			gl::color(
				//(color * ((double)(i + source->_num_threads) / (double)(source->_num_threads * 2)))
				(color * ((double)1 - ((double)i / ((double) source->_num_threads * 2))))
				.toCinderColor());

			draw::drawRect(
				var::coord2(0, (double)sect_top - (thread_box_ySize * (double)i)),
				var::coord2(xSize, (double)sect_top - (thread_box_ySize * (double)(i + 1)) + buffer_size)
			);

			draw::drawStringRight(conv::toString(source->_num_threads) + " thread" + ((source->_num_threads == 1) ? "" : "s"), var::coord2(100, sect_top), false, FONT_SIZE, var::color_RGB::GREEN());
		}
	}
}

void multiSort_GraphicalApp::draw()
{
	gl::clear(Color(0, 0, 0));

	//Draw the background
	{
		auto drawBars = [](vector<pair<int, int>> barz)
		{
			for(unsigned i = 0; i < barz.size(); i++)
			{
				if(i % 2) gl::color(Color(.25, .25, .25));
				else gl::color(Color(.1, .1, .1));

				//For some reason the compiler won't accept shortening thse onto one line
				pair<int, int> cBar;
				cBar = barz[i];

				var::coord2 first(0, cBar.first);
				var::coord2 second(100, cBar.second);

				draw::drawRect(first, second);
			}
		};

		vector<pair<int, int>> bars;
		bars.push_back({53, 40});
		bars.push_back({40, 35});
		bars.push_back({35, 30});
		bars.push_back({30, 25});
		bars.push_back({25, 20});
		bars.push_back({20, 15});
		bars.push_back({15, 0});

		drawBars(bars);
	}


	runtimeStats * statRef = & sortDemo::multiSort::stats;
	
	
	{	//Chart thread runtimes
		//Currently executing one
		drawThreadProgressBars(statRef, 53, 40, var::color_RGB::GREEN());

		//The rest
		drawThreadProgressBars(&data::stats_8thr, 35, 30);
		drawThreadProgressBars(&data::stats_4thr, 30, 25);
		drawThreadProgressBars(&data::stats_2thr, 25, 20);
		drawThreadProgressBars(&data::stats_1thr, 20, 15);
	}


	chartDataset(sortDemo::dataset);

	{
		const unsigned numSegs = 10;
		double chunkX = 100 / numSegs;
		for(unsigned i = 1; i < numSegs; i++)
		{
			draw::drawRect(var::coord2(i * chunkX - window::unscaleX(5), (40)), var::coord2(i * chunkX + window::unscaleX(5), (35)), var::coord2(), 0, false, false, var::color_RGB::GREEN());

			unsigned stringY = (i % 2 == 0) ? 40 : 38;

			draw::drawStringCentered(conv::toString((i * chunkX /10) / (timeScale)) + " seconds", var::coord2(i * chunkX, (stringY)), false, FONT_SIZE, var::color_RGB::BLUE());


		}


	}
	draw::drawStringLeft("Dataset Visualization", var::coord2(0, 15), false, FONT_SIZE, var::color_RGB::BLUE());

	draw::drawStringCentered("Sorting algorithm execution time vs. Number of threads used", var::coord2(50, 57), false, FONT_SIZE, var::color_RGB::GREEN());
	
	gl::color(Color(0, 0, 0));
	//draw::drawStringCentered(data::sortStatus, var::coord2(50, 50));


}

CINDER_APP( multiSort_GraphicalApp, RendererGl )
