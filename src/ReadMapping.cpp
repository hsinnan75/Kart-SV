#include "structure.h"
#include "htslib/htslib/sam.h"
#include "sam_opts.h"
#include "htslib/htslib/kseq.h"
#include "htslib/htslib/kstring.h"

#define MinInversionSize 1000
#define MaxPairedDistance 2000
#define MaxInversionSize 10000000
#define MinTranslocationSize 1000

FILE *vcf_output;
FILE *sam_out = 0;
samFile *bam_out = 0;
bam_hdr_t *header = NULL;
bool bSepLibrary = false;
FILE *ReadFileHandler1, *ReadFileHandler2;
gzFile gzReadFileHandler1, gzReadFileHandler2;
vector<DiscordPair_t> InversionSiteVec, TranslocationSiteVec;
uint32_t avgCov, avgReadLength, avgDist = 1000;
int64_t iTotalReadNum = 0, iTotalMappingNum = 0, iTotalPairedNum = 0, iAlignedBase = 0, iTotalCoverage = 0, TotalPairedDistance = 0, ReadLengthSum = 0;

void ShowMappedRegion(vector<FragPair_t>& FragPairVec)
{
	int TailIdx = (int)FragPairVec.size() - 1;
	printf("Coor=%lld - %lld\n", (long long)DetermineCoordinate(FragPairVec[0].gPos).gPos, (long long)DetermineCoordinate(FragPairVec[TailIdx].gPos + FragPairVec[TailIdx].gLen).gPos);
}

void ShowFragPairCluster(vector<AlnCan_t>& AlnCanVec)
{
	int i, num = (int)AlnCanVec.size();

	printf("AlnCan#=%d\n", num);
	for (i = 0; i < num; i++)
	{
		if (AlnCanVec[i].score == 0) continue;
		printf("AlnCan#%d: score = %d (%d)\n", i + 1, AlnCanVec[i].score, AlnCanVec[i].PairedAlnCanIdx + 1);
		ShowSimplePairInfo(AlnCanVec[i].FragPairVec);
	}
	printf("%s\n", string().assign(50, '*').c_str());
}

bool CompByPosDiff(const FragPair_t& p1, const FragPair_t& p2)
{
	if (p1.PosDiff == p2.PosDiff) return p1.rPos < p2.rPos;
	else return p1.PosDiff < p2.PosDiff;
}

bool CompByAlnCanScore(const AlnCan_t& p1, const AlnCan_t& p2)
{
	if (p1.score == p2.score) return p1.FragPairVec[0].PosDiff < p2.FragPairVec[0].PosDiff;
	else return p1.score > p2.score;
}

bool CompByDiscordPos(const DiscordPair_t& p1, const DiscordPair_t& p2)
{
	return p1.gPos < p2.gPos;
}

bool CheckMultiAlnCan(vector<AlnCan_t>& AlnCanVec)
{
	int num = 0;

	for (vector<AlnCan_t>::iterator iter = AlnCanVec.begin(); iter != AlnCanVec.end(); iter++) if (iter->score > 0) num++;
	if (num > 1) return true;
	else return false;
}

void ResetPairedIdx(vector<AlnCan_t>& AlnCanVec)
{
	for (vector<AlnCan_t>::iterator iter = AlnCanVec.begin(); iter != AlnCanVec.end(); iter++) iter->PairedAlnCanIdx = -1;
}

static bam_hdr_t *sam_hdr_sanitise(bam_hdr_t *h)
{
	if (!h) return NULL;
	if (h->l_text == 0) return h;

	uint32_t i;
	char *cp = h->text;
	for (i = 0; i < h->l_text; i++) {
		// NB: l_text excludes terminating nul.  This finds early ones.
		if (cp[i] == 0) break;
	}
	if (i < h->l_text) { // Early nul found.  Complain if not just padding.
		uint32_t j = i;
		while (j < h->l_text && cp[j] == '\0') j++;
	}
	return h;
}

bam_hdr_t *SamHdr2BamHdr(kstring_t *str)
{
	bam_hdr_t *h = NULL;
	h = sam_hdr_parse(str->l, str->s);
	h->l_text = str->l; h->text = str->s;

	return sam_hdr_sanitise(h);
}

void OutputSamHeaders()
{
	int i, len;
	char buffer[1024];
	kstring_t str = { 0, 0, NULL };

	len = sprintf(buffer, "@PG\tID:MapCaller\tPN:MapCaller\tVN:%s\n", VersionStr);

	if(bSAMFormat) fprintf(sam_out, "%s", buffer);
	else kputsn(buffer, len, &str);

	for (i = 0; i < iChromsomeNum; i++)
	{
		len = sprintf(buffer, "@SQ\tSN:%s\tLN:%d\n", ChromosomeVec[i].name, ChromosomeVec[i].len);
		if (bSAMFormat) fprintf(sam_out, "%s", buffer);
		else kputsn(buffer, len, &str);
	}
	if (!bSAMFormat)
	{
		header = SamHdr2BamHdr(&str);
		sam_hdr_write(bam_out, header);
	}
}

vector<FragPair_t> IdentifySimplePairs(int rlen, uint8_t* EncodeSeq)
{
	int i, pos, stop_pos;
	FragPair_t FragPair;
	vector<FragPair_t> SPvec;
	bwtSearchResult_t bwtSearchResult;

	FragPair.bSimple = true; pos = 0; stop_pos = rlen - MinSeedLength;
	while (pos < stop_pos)
	{
		if (EncodeSeq[pos] > 3) pos++;
		else
		{
			bwtSearchResult = BWT_Search(EncodeSeq, pos, rlen);
			if (bwtSearchResult.freq > 0)
			{
				FragPair.rPos = pos; FragPair.rLen = FragPair.gLen = bwtSearchResult.len;
				for (i = 0; i != bwtSearchResult.freq; i++)
				{
					FragPair.PosDiff = (FragPair.gPos = bwtSearchResult.LocArr[i]) - FragPair.rPos;
					if (FragPair.PosDiff > 0) SPvec.push_back(FragPair);
				}
				delete[] bwtSearchResult.LocArr;
			}
			pos += (bwtSearchResult.len + 1);
		}
	}
	sort(SPvec.begin(), SPvec.end(), CompByPosDiff);

	FragPair.rLen = FragPair.gLen = 0; FragPair.gPos = FragPair.PosDiff = TwoGenomeSize;
	SPvec.push_back(FragPair); // add a terminal fragment pair

	return SPvec;
}

AlnCan_t IdentifyClosestFragmentPairs(int BegIdx, int EndIdx, vector<FragPair_t>& SimplePairVec)
{
	int i, j, s;
	AlnCan_t AlnCan;
	pair<int, int> Boudnary;

	AlnCan.score = 0; i = BegIdx; s = SimplePairVec[BegIdx].rLen;
	for (j = BegIdx + 1; j < EndIdx; j++)
	{
		if (SimplePairVec[j].PosDiff != SimplePairVec[i].PosDiff)
		{
			if (s > AlnCan.score)
			{
				AlnCan.score = s;
				Boudnary = make_pair(i, j);
			}
			i = j; s = SimplePairVec[j].rLen;
		}
		else s += SimplePairVec[j].rLen;
	}
	if (s > AlnCan.score) // last checks
	{
		AlnCan.score = s;
		Boudnary = make_pair(i, j);
	}
	copy(SimplePairVec.begin() + Boudnary.first, SimplePairVec.begin() + Boudnary.second, back_inserter(AlnCan.FragPairVec));
	//if (bDebugMode)
	//{
	//	printf("Best AlnCan: score=%d\n", AlnCan.score);
	//	ShowSimplePairInfo(AlnCan.FragPairVec);
	//}
	return AlnCan;
}

vector<AlnCan_t> SimplePairClustering(int rlen, vector<FragPair_t>& SimplePairVec)
{
	AlnCan_t AlnCan;
	int64_t gPos_end;
	vector<AlnCan_t> AlnCanVec;
	int i, j, score_thr, HeadIdx, num = (int)SimplePairVec.size();

	HeadIdx = 0; gPos_end = GetAlignmentBoundary(SimplePairVec[0].gPos); AlnCan.score = SimplePairVec[0].rLen; score_thr = (rlen >> 2);
	for (i = 0, j = 1; j < num; i++, j++)
	{
		if (SimplePairVec[j].gPos > gPos_end || abs(SimplePairVec[j].PosDiff - SimplePairVec[i].PosDiff) > MaxPosDiff)
		{
			if (AlnCan.score > score_thr)
			{
				if (score_thr < (AlnCan.score >> 1)) score_thr = (AlnCan.score >> 1);
				if (AlnCan.score >= rlen) // tandem repeats!!
				{
					AlnCan = IdentifyClosestFragmentPairs(HeadIdx, j, SimplePairVec);
					AlnCanVec.push_back(AlnCan); AlnCan.FragPairVec.clear();
				}
				else
				{
					copy(SimplePairVec.begin() + HeadIdx, SimplePairVec.begin() + j, back_inserter(AlnCan.FragPairVec));
					AlnCanVec.push_back(AlnCan); AlnCan.FragPairVec.clear();
				}
			}
			HeadIdx = j; gPos_end = GetAlignmentBoundary(SimplePairVec[j].gPos); AlnCan.score = SimplePairVec[j].rLen;
		}
		else AlnCan.score += SimplePairVec[j].rLen;
	}
	//sort(AlnCanVec.begin(), AlnCanVec.end(), CompByAlnCanScore);
	return AlnCanVec;
}

void RemoveRedundantAlnCan(vector<AlnCan_t>& AlnCanVec)
{
	if ((int)AlnCanVec.size() > 1)
	{
		int n, maxScore = 0;
		vector<AlnCan_t>::iterator iter;

		for (iter = AlnCanVec.begin(); iter != AlnCanVec.end(); iter++)
		{
			if (iter->score > maxScore) n = 1, maxScore = iter->score;
			else if (iter->score == maxScore) n++;
		}
		for (iter = AlnCanVec.begin(); iter != AlnCanVec.end(); iter++) if (iter->score < maxScore) iter->score = 0;
	}
}

int CheckPairedAlignmentDistance(int64_t EstiDistance, vector<AlnCan_t>& AlnCanVec1, vector<AlnCan_t>& AlnCanVec2)
{
	PairedReads_t PairedReads;
	vector<PairedReads_t> PairedIdxVec;
	int i, j, num1, num2, paired_num = 0;
	vector<FragPair_t>::iterator FragPairIter1, FragPairIter2;
	int64_t myDist, max_score;

	num1 = (int)AlnCanVec1.size(); num2 = (int)AlnCanVec2.size(); max_score = 0;

	if (num1*num2 > 100)
	{
		RemoveRedundantAlnCan(AlnCanVec1);
		RemoveRedundantAlnCan(AlnCanVec2);
	}
	for (i = 0; i != num1; i++)
	{
		if (AlnCanVec1[i].score == 0) continue;

		for (PairedReads.idx1 = i, PairedReads.idx2=-1, PairedReads.p_score = 0, j=0; j != num2; j++)
		{
			if (AlnCanVec2[j].score == 0 || AlnCanVec2[j].FragPairVec[0].PosDiff < AlnCanVec1[i].FragPairVec[0].PosDiff) continue;
			if ((myDist = AlnCanVec2[j].FragPairVec[0].PosDiff - AlnCanVec1[i].FragPairVec[0].PosDiff) < EstiDistance)
			{
				if (AlnCanVec2[j].score > PairedReads.p_score)
				{
					PairedReads.idx2 = j;
					PairedReads.p_score = AlnCanVec2[j].score;
				}
			}
		}
		if (PairedReads.idx2 != -1)
		{
			PairedReads.p_score = AlnCanVec1[i].score + AlnCanVec2[PairedReads.idx2].score;
			if (PairedReads.p_score > max_score)
			{
				max_score = PairedReads.p_score;
				PairedIdxVec.push_back(PairedReads);
			}
			else if (PairedReads.p_score == max_score)
			{
				PairedIdxVec.push_back(PairedReads);
			}
		}
	}
	if (max_score > 0)
	{
		for (vector<PairedReads_t>::iterator iter = PairedIdxVec.begin(); iter != PairedIdxVec.end(); iter++)
		{
			if (iter->p_score == max_score)
			{
				paired_num++;
				//if (bDebugMode) printf("PairedAlnCan = %d and %d\n", iter->idx1 + 1, iter->idx2 + 1);
				AlnCanVec1[iter->idx1].PairedAlnCanIdx = iter->idx2;
				AlnCanVec2[iter->idx2].PairedAlnCanIdx = iter->idx1;
			}
		}
	}
	return paired_num;
}

void MaskUnPairedAlnCan(vector<AlnCan_t>& AlnCanVec1, vector<AlnCan_t>& AlnCanVec2)
{
	int max_score = 0;
	vector<AlnCan_t>::iterator iter;

	for (iter = AlnCanVec1.begin(); iter != AlnCanVec1.end(); iter++)
	{
		if (iter->PairedAlnCanIdx != -1 && max_score < (iter->score + AlnCanVec2[iter->PairedAlnCanIdx].score)) max_score = iter->score + AlnCanVec2[iter->PairedAlnCanIdx].score;
	}
	for (iter = AlnCanVec1.begin(); iter != AlnCanVec1.end(); iter++)
	{
		if (iter->PairedAlnCanIdx == -1 || (iter->score + AlnCanVec2[iter->PairedAlnCanIdx].score) < max_score) iter->score = 0;
	}
	for (iter = AlnCanVec2.begin(); iter != AlnCanVec2.end(); iter++)
	{
		if (iter->PairedAlnCanIdx == -1 || (iter->score + AlnCanVec1[iter->PairedAlnCanIdx].score) < max_score) iter->score = 0;
	}
}


bool CheckAlignmentQuality(FragPair_t& FragPair)
{
	int i, len, iMis, iGap;
	bool bPass = true;

	len = (int)FragPair.aln1.length(); iMis = iGap = 0;
	for (i = 0; i < len; i++)
	{
		if (FragPair.aln1[i] == '-' || FragPair.aln2[i] == '-') iGap++;
		else if (FragPair.aln1[i] != FragPair.aln2[i]) iMis++;
	}
	if (iMis > 0 && iGap > 0) bPass = false;
	else if (iMis > 1 && iMis > (int)(len*0.2)) bPass = false;

	return bPass;
}


CoordinatePair_t GetPairedAlnCanDist(vector<AlnCan_t>& AlnCanVec1, vector<AlnCan_t>& AlnCanVec2)
{
	CoordinatePair_t CoorPair;

	CoorPair.dist = 0;
	for (vector<AlnCan_t>::iterator iter = AlnCanVec1.begin(); iter != AlnCanVec1.end(); iter++)
	{
		if (iter->score > 0 && iter->PairedAlnCanIdx != -1 && AlnCanVec2[iter->PairedAlnCanIdx].score > 0)
		{
			CoorPair.gPos1 = iter->FragPairVec[0].gPos;
			CoorPair.gPos2 = AlnCanVec2[iter->PairedAlnCanIdx].FragPairVec[0].gPos;
			CoorPair.dist = abs(CoorPair.gPos2 - CoorPair.gPos1);
			break;
		}
	}
	return CoorPair;
}

CoordinatePair_t GenCoordinatePair(vector<AlnCan_t>& AlnCanVec1, vector<AlnCan_t>& AlnCanVec2)
{
	int num1, num2;
	CoordinatePair_t CoorPair;
	vector<AlnCan_t>::iterator iter;
	vector<int64_t> gPosVec1, gPosVec2;

	if ((CoorPair = GetPairedAlnCanDist(AlnCanVec1, AlnCanVec2)).dist != 0) return CoorPair;
	else
	{
		for (iter = AlnCanVec1.begin(); iter != AlnCanVec1.end(); iter++) if (iter->score > 0) gPosVec1.push_back(iter->FragPairVec[0].gPos);
		for (iter = AlnCanVec2.begin(); iter != AlnCanVec2.end(); iter++) if (iter->score > 0) gPosVec2.push_back(iter->FragPairVec[0].gPos);

		num1 = (int)gPosVec1.size(); num2 = (int)gPosVec2.size();
		if (num1 == 1 && num2 == 1) // discordant
		{
			CoorPair.gPos1 = gPosVec1[0];
			CoorPair.gPos2 = gPosVec2[0];
			CoorPair.dist = abs(CoorPair.gPos2 - CoorPair.gPos1);
		}
		else if (num1 == 0 && num2 >= 1) // OEA
		{
			CoorPair.gPos1 = -1;
			CoorPair.dist = CoorPair.gPos2 = gPosVec2[0];
		}
		else if (num1 >= 1 && num2 == 0) // OEA
		{
			CoorPair.dist = CoorPair.gPos1 = gPosVec1[0];
			CoorPair.gPos2 = -1;
		}
		else CoorPair.dist = 0;
	}
	return CoorPair;
}

int CheckAlnNumber(vector<AlnCan_t>& AlnCanVec)
{
	int n = 0;
	for (vector<AlnCan_t>::iterator iter = AlnCanVec.begin(); iter != AlnCanVec.end(); iter++) if (iter->score > 0) n++;
	return n;
	//return n == 1 ? true : false;
}

void EnCodeReadSeq(int rlen, char* seq, uint8_t* EncodeSeq)
{
	for (int i = 0; i < rlen; i++) EncodeSeq[i] = nst_nt4_table[(int)seq[i]];
}

void FreeReadItem(ReadItem_t* read)
{
	if (read->header != NULL) delete[] read->header;
	if (read->seq != NULL)delete[] read->seq;
	if (read->qual != NULL)delete[] read->qual;
}

void *ReadMapping(void *arg)
{
	uint8_t* EncodeSeq;
	AlnSummary_t AlnSummary;
	DiscordPair_t DiscordPair;
	CoordinatePair_t CoorPair;
	ReadItem_t* ReadArr = NULL;
	vector<string> SamStreamVec;
	vector<FragPair_t> SimplePairVec;
	int64_t myTotalDistance, myReadLengthSum;
	int i, j, n, ReadNum, MappedNum, PairedNum;
	vector<DiscordPair_t> INVSiteVec , TNLSiteVec;

	ReadArr = new ReadItem_t[ReadChunkSize];

	AlnSummary.score = AlnSummary.sub_score = 0; AlnSummary.BestAlnCanIdx = -1;
	while (true)
	{
		pthread_mutex_lock(&LibraryLock);
		if (gzCompressed) ReadNum = gzGetNextChunk(bSepLibrary, gzReadFileHandler1, gzReadFileHandler2, ReadArr);
		else ReadNum = GetNextChunk(bSepLibrary, ReadFileHandler1, ReadFileHandler2, ReadArr);
		fprintf(stderr, "\r%lld %s reads have been processed in %lld seconds...", (long long)iTotalReadNum, (bPairEnd ? "paired-end" : "singled-end"), (long long)(time(NULL) - StartProcessTime));
		pthread_mutex_unlock(&LibraryLock);

		if (ReadNum == 0) break;

		if (bPairEnd && ReadNum % 2 == 0)
		{
			MappedNum = PairedNum = 0; myTotalDistance = myReadLengthSum = 0;
			for (i = 0, j = 1; i != ReadNum; i += 2, j += 2)
			{
				EncodeSeq = new uint8_t[ReadArr[i].rlen]; EnCodeReadSeq(ReadArr[i].rlen, ReadArr[i].seq, EncodeSeq);
				SimplePairVec = IdentifySimplePairs(ReadArr[i].rlen, EncodeSeq); delete[] EncodeSeq;
				ReadArr[i].AlnCanVec = SimplePairClustering(ReadArr[i].rlen, SimplePairVec);

				ReverseOrientation(&ReadArr[j]);
				EncodeSeq = new uint8_t[ReadArr[j].rlen]; EnCodeReadSeq(ReadArr[j].rlen, ReadArr[j].seq, EncodeSeq);
				SimplePairVec = IdentifySimplePairs(ReadArr[j].rlen, EncodeSeq); delete[] EncodeSeq;
				ReadArr[j].AlnCanVec = SimplePairClustering(ReadArr[j].rlen, SimplePairVec);

				ReadArr[i].AlnSummary = AlnSummary; ReadArr[j].AlnSummary = AlnSummary;
				ResetPairedIdx(ReadArr[i].AlnCanVec); ResetPairedIdx(ReadArr[j].AlnCanVec);

				//printf("read:%s\n", ReadArr[i].header); ShowFragPairCluster(ReadArr[i].AlnCanVec);
				//printf("read:%s\n", ReadArr[j].header); ShowFragPairCluster(ReadArr[j].AlnCanVec);

				n = CheckPairedAlignmentDistance((int)(avgDist*1.5), ReadArr[i].AlnCanVec, ReadArr[j].AlnCanVec);
				if (n == 0) n = AlignmentRescue((int)(avgDist*1.5), ReadArr[i], ReadArr[j]); // perform alignment rescue
				//if (bDebugMode)
				//{
				//	printf("read1:%s\n", ReadArr[i].header); ShowFragPairCluster(ReadArr[i].AlnCanVec);
				//	printf("read2:%s\n", ReadArr[j].header); ShowFragPairCluster(ReadArr[j].AlnCanVec);
				//}
				if(n == 0) RemoveRedundantAlnCan(ReadArr[i].AlnCanVec), RemoveRedundantAlnCan(ReadArr[j].AlnCanVec);
				else MaskUnPairedAlnCan(ReadArr[i].AlnCanVec, ReadArr[j].AlnCanVec);

				if (ProduceReadAlignment(ReadArr[i])) MappedNum++;
				if (ProduceReadAlignment(ReadArr[j])) MappedNum++;
				//if (bDebugMode)
				//{
				//	printf("read1:%s\n", ReadArr[i].header); ShowFragPairCluster(ReadArr[i].AlnCanVec);
				//	printf("read2:%s\n", ReadArr[j].header); ShowFragPairCluster(ReadArr[j].AlnCanVec);
				//}
				if ((CoorPair = GenCoordinatePair(ReadArr[i].AlnCanVec, ReadArr[j].AlnCanVec)).dist != 0)
				{
					if (CoorPair.gPos1 == -1 || CoorPair.gPos2 == -1)
					{
					}
					else
					{
						if ((CoorPair.gPos1 < GenomeSize && CoorPair.gPos2 >= GenomeSize))
						{
							if (bVCFoutput)
							{
								DiscordPair.dist = abs(TwoGenomeSize - CoorPair.gPos1 - CoorPair.gPos2);
								if (DiscordPair.dist > MinInversionSize && DiscordPair.dist < MaxInversionSize)
								{
									DiscordPair.gPos = CoorPair.gPos1; INVSiteVec.push_back(DiscordPair);
								}
							}
						}
						else if (CoorPair.gPos1 >= GenomeSize && CoorPair.gPos2 < GenomeSize)
						{
							if (bVCFoutput)
							{
								DiscordPair.dist = abs(TwoGenomeSize - CoorPair.gPos1 - CoorPair.gPos2);
								if (DiscordPair.dist > MinInversionSize && DiscordPair.dist < MaxInversionSize) DiscordPair.gPos = CoorPair.gPos2; INVSiteVec.push_back(DiscordPair);
							}
						}
						else if (CoorPair.dist > MinTranslocationSize)
						{
							if (bVCFoutput)
							{
								DiscordPair.dist = CoorPair.dist;
								//printf("gPos1=%lld, gPos2=%lld, dist=%lld\n", CoorPair.gPos1, CoorPair.gPos2, CoorPair.dist);
								if (CoorPair.gPos1 < GenomeSize && CoorPair.gPos2 < GenomeSize)
								{
									DiscordPair.gPos = CoorPair.gPos1; TNLSiteVec.push_back(DiscordPair);
									DiscordPair.gPos = CoorPair.gPos2; TNLSiteVec.push_back(DiscordPair);
								}
								else if (CoorPair.gPos1 >= GenomeSize && CoorPair.gPos2 >= GenomeSize)
								{
									DiscordPair.gPos = TwoGenomeSize - CoorPair.gPos1; TNLSiteVec.push_back(DiscordPair);
									DiscordPair.gPos = TwoGenomeSize - CoorPair.gPos2; TNLSiteVec.push_back(DiscordPair);
								}
							}
						}
						else
						{
							//Coordinate_t coor1 = DetermineCoordinate(CoorPair.gPos1), coor2 = DetermineCoordinate(CoorPair.gPos2);
							//MC = GenMappingCoordinates(CoorPair);
							//MC.rlen1 = ReadArr[i].rlen; MC.rlen2 = ReadArr[j].rlen;
							//printf("%lld %lld vs %d %d\n", (long long)coor1.gPos - 1, (long long)coor2.gPos - 1, MC.gPos1, MC.gPos2);
							myReadLengthSum += ReadArr[i].rlen;
							myReadLengthSum += ReadArr[j].rlen;
							PairedNum++; myTotalDistance += CoorPair.dist;
						}
					}
				}
			}
			if (bSAMoutput) for (SamStreamVec.clear(), i = 0, j = 1; i != ReadNum; i += 2, j += 2) GeneratePairedSamStream(ReadArr[i], ReadArr[j], SamStreamVec);
			pthread_mutex_lock(&OutputLock);
			iTotalReadNum += ReadNum; iTotalMappingNum += MappedNum; iTotalPairedNum += PairedNum; TotalPairedDistance += myTotalDistance, ReadLengthSum += myReadLengthSum;
			if (iTotalPairedNum > 1000) avgDist = (int)(1.*TotalPairedDistance / iTotalPairedNum + .5);

			if (bSAMoutput)
			{
				if (bSAMFormat)
				{
					for (vector<string>::iterator iter = SamStreamVec.begin(); iter != SamStreamVec.end(); iter++) fprintf(sam_out, "%s\n", iter->c_str());
					fflush(sam_out);
				}
				else
				{
					bam1_t *b = bam_init1();
					kstring_t str = { 0, 0, NULL };
					for (vector<string>::iterator iter = SamStreamVec.begin(); iter != SamStreamVec.end(); iter++)
					{
						str.s = (char*)iter->c_str(); str.l = iter->length();
						if (sam_parse1(&str, header, b) >= 0) sam_write1(bam_out, header, b);
					}
					bam_destroy1(b);
				}
			}
			pthread_mutex_unlock(&OutputLock);
			// update the mapping status and the query genome profile
			if (bVCFoutput) 
			{
				pthread_mutex_lock(&ProfileLock);
				for (i = 0; i != ReadNum; i++)
				{
					//if (strcmp(ReadArr[i].header, "NC_000913_mut_1472703_1473141_0_1_0_0_0:0:0_2:0:0_45ec") == 0) ShowFragPairCluster(ReadArr[i].AlnCanVec);
					if (ReadArr[i].AlnSummary.score == 0) continue;
					if ((n = CheckAlnNumber(ReadArr[i].AlnCanVec)) == 1) UpdateProfile((i % 2 == 0), ReadArr + i, ReadArr[i].AlnCanVec);
					else UpdateMultiHitCount(ReadArr + i, ReadArr[i].AlnCanVec);
				}
				pthread_mutex_unlock(&ProfileLock);
			}
		}
		else //singled-end reads
		{
			MappedNum = 0;
			for (i = 0; i != ReadNum; i++)
			{
				EncodeSeq = new uint8_t[ReadArr[i].rlen]; EnCodeReadSeq(ReadArr[i].rlen, ReadArr[i].seq, EncodeSeq);
				SimplePairVec = IdentifySimplePairs(ReadArr[i].rlen, EncodeSeq); delete[] EncodeSeq;
				ReadArr[i].AlnSummary = AlnSummary; ReadArr[i].AlnCanVec = SimplePairClustering(ReadArr[i].rlen, SimplePairVec);
				RemoveRedundantAlnCan(ReadArr[i].AlnCanVec); 
				if (ProduceReadAlignment(ReadArr[i])) MappedNum++;
			}
			if (bSAMoutput) for (SamStreamVec.clear(), i = 0; i != ReadNum; i++) GenerateSingleSamStream(ReadArr[i], SamStreamVec);
			pthread_mutex_lock(&OutputLock);
			iTotalReadNum += ReadNum; iTotalMappingNum += MappedNum;
			if (bSAMoutput)
			{
				if (bSAMFormat)
				{
					for (vector<string>::iterator iter = SamStreamVec.begin(); iter != SamStreamVec.end(); iter++) fprintf(sam_out, "%s\n", iter->c_str());
					fflush(sam_out);
				}
				else
				{
					bam1_t *b = bam_init1();
					kstring_t str = { 0, 0, NULL };
					for (vector<string>::iterator iter = SamStreamVec.begin(); iter != SamStreamVec.end(); iter++)
					{
						str.s = (char*)iter->c_str(); str.l = iter->length();
						if (sam_parse1(&str, header, b) >= 0) sam_write1(bam_out, header, b);
					}
					bam_destroy1(b);
				}
			}
			pthread_mutex_unlock(&OutputLock);
			// update the mapping status and the query genome profile
			if (bVCFoutput)
			{
				pthread_mutex_lock(&ProfileLock);
				for (i = 0; i != ReadNum; i++)
				{
					if (ReadArr[i].AlnSummary.score == 0) continue;
					if ((n = CheckAlnNumber(ReadArr[i].AlnCanVec)) == 1) UpdateProfile(true, ReadArr + i, ReadArr[i].AlnCanVec);
					else UpdateMultiHitCount(ReadArr + i, ReadArr[i].AlnCanVec);
				}
				pthread_mutex_unlock(&ProfileLock);
			}
		}
		for (i = 0; i != ReadNum; i++)  FreeReadItem(ReadArr + i);
		//if (iTotalReadNum >= 10000) break;
	}
	delete[] ReadArr;

	if (bVCFoutput)
	{
		sort(TNLSiteVec.begin(), TNLSiteVec.end(), CompByDiscordPos);
		sort(INVSiteVec.begin(), INVSiteVec.end(), CompByDiscordPos);

		pthread_mutex_lock(&ProfileLock);
		if ((n = (int)TNLSiteVec.size()) > 0)
		{
			copy(TNLSiteVec.begin(), TNLSiteVec.end(), back_inserter(TranslocationSiteVec));
			inplace_merge(TranslocationSiteVec.begin(), TranslocationSiteVec.end() - n, TranslocationSiteVec.end(), CompByDiscordPos);
		}
		if ((n = (int)INVSiteVec.size()) > 0)
		{
			copy(INVSiteVec.begin(), INVSiteVec.end(), back_inserter(InversionSiteVec));
			inplace_merge(InversionSiteVec.begin(), InversionSiteVec.end() - n, InversionSiteVec.end(), CompByDiscordPos);
		}
		pthread_mutex_unlock(&ProfileLock);
	}
	return (void*)(1);
}

void *CheckMappingCoverage(void *arg)
{
	int cov, tid = *((int*)arg);
	int64_t gPos, myAlignedBase = 0, myCoverageSum = 0;

	for (gPos = tid; gPos < GenomeSize; gPos += iThreadNum)
	{
		if ((cov = GetProfileColumnSize(MappingRecordArr[gPos])) > 0)
		{
			myAlignedBase++;
			myCoverageSum += cov;
			//if (cov > 45) printf("%lld, %d\n", gPos, cov);
		}
	}
	pthread_mutex_lock(&ProfileLock);
	iAlignedBase += myAlignedBase;
	iTotalCoverage += myCoverageSum;
	pthread_mutex_unlock(&ProfileLock);

	return (void*)(1);
}

pair<int64_t, int64_t> ReportDuplicationRate()
{
	int64_t gPos, n, total_count;

	n = total_count = 0;

	for (gPos = 0; gPos < GenomeSize; gPos++)
	{
		if (MappingRecordArr[gPos].readCount > 0)
		{
			n++;
			total_count += MappingRecordArr[gPos].readCount;
		}
	}
	total_count -= n;

	return make_pair(total_count, n);
}

void Mapping()
{
	FILE *log;
	int i, *ThrIdArr;
	pthread_t *ThreadArr = new pthread_t[iThreadNum];

	//iThreadNum = 1;
	ThrIdArr = new int[iThreadNum];  for (i = 0; i < iThreadNum; i++) ThrIdArr[i] = i;

	if (bSAMoutput && SamFileName != NULL)
	{
		if (bSAMFormat) sam_out = strcmp(SamFileName, "-") == 0 ? fopen("/dev/stdout", "w") : fopen(SamFileName, "w");
		else bam_out = strcmp(SamFileName, "-") == 0 ? sam_open_format("/dev/stdout", "wb", NULL): sam_open_format(SamFileName, "wb", NULL);
	}
	if (bSAMoutput) OutputSamHeaders();

	for (int LibraryID = 0; LibraryID < (int)ReadFileNameVec1.size(); LibraryID++)
	{
		gzReadFileHandler1 = gzReadFileHandler2 = NULL; ReadFileHandler1 = ReadFileHandler2 = NULL;

		if (ReadFileNameVec1[LibraryID].substr(ReadFileNameVec1[LibraryID].find_last_of('.') + 1) == "gz") gzCompressed = true;
		else gzCompressed = false;

		FastQFormat = CheckReadFormat(ReadFileNameVec1[LibraryID].c_str());
		if (gzCompressed) gzReadFileHandler1 = gzopen(ReadFileNameVec1[LibraryID].c_str(), "rb");
		else ReadFileHandler1 = fopen(ReadFileNameVec1[LibraryID].c_str(), "r");

		if (ReadFileNameVec1.size() == ReadFileNameVec2.size())
		{
			bSepLibrary = bPairEnd = true;
			if (FastQFormat == CheckReadFormat(ReadFileNameVec2[LibraryID].c_str()))
			{
				if (gzCompressed) gzReadFileHandler2 = gzopen(ReadFileNameVec2[LibraryID].c_str(), "rb");
				else ReadFileHandler2 = fopen(ReadFileNameVec2[LibraryID].c_str(), "r");
			}
			else
			{
				fprintf(stderr, "Error! %s and %s are with different format...\n", (char*)ReadFileNameVec1[LibraryID].c_str(), (char*)ReadFileNameVec2[LibraryID].c_str());
				continue;
			}
		}
		else bSepLibrary = false;

		if (ReadFileHandler1 == NULL && gzReadFileHandler1 == NULL) continue;
		if (bSepLibrary && ReadFileHandler2 == NULL && gzReadFileHandler2 == NULL) continue;

		for (i = 0; i < iThreadNum; i++) pthread_create(&ThreadArr[i], NULL, ReadMapping, &ThrIdArr[i]);
		for (i = 0; i < iThreadNum; i++) pthread_join(ThreadArr[i], NULL);

		if (gzCompressed)
		{
			if (gzReadFileHandler1 != NULL) gzclose(gzReadFileHandler1);
			if (gzReadFileHandler2 != NULL) gzclose(gzReadFileHandler2);
		}
		else
		{
			if (ReadFileHandler1 != NULL) fclose(ReadFileHandler1);
			if (ReadFileHandler2 != NULL) fclose(ReadFileHandler2);
		}
	}
	log = fopen(LogFileName, "a");
	fprintf(log, "All the %lld %s reads have been processed in %lld seconds.\n", (long long)iTotalReadNum, (bPairEnd ? "paired-end" : "single-end"), (long long)(time(NULL) - StartProcessTime));
	fprintf(stderr, "\rAll the %lld %s reads have been processed in %lld seconds.\n", (long long)iTotalReadNum, (bPairEnd ? "paired-end" : "single-end"), (long long)(time(NULL) - StartProcessTime));
	if (iTotalReadNum > 0)
	{
		fprintf(log, "%12lld (%6.2f%%) reads are mapped properly.\n", (long long)iTotalMappingNum, (int)(10000 * (1.0*iTotalMappingNum / iTotalReadNum) + 0.00005) / 100.0);
		fprintf(stderr, "%12lld (%6.2f%%) reads are mapped properly.\n", (long long)iTotalMappingNum, (int)(10000 * (1.0*iTotalMappingNum / iTotalReadNum) + 0.00005) / 100.0);
	}
	if (iTotalReadNum > 0 && iTotalPairedNum > 0)
	{
		fprintf(log, "%12lld (%6.2f%%) reads are mapped in pairs.\n", (long long)(iTotalPairedNum << 1), (int)(10000 * (1.0*(iTotalPairedNum << 1) / iTotalReadNum) + 0.00005) / 100.0);
		fprintf(stderr, "%12lld (%6.2f%%) reads are mapped in pairs.\n", (long long)(iTotalPairedNum << 1), (int)(10000 * (1.0*(iTotalPairedNum << 1) / iTotalReadNum) + 0.00005) / 100.0);
	}
	if (bSAMoutput)
	{
		if (bSAMFormat) fclose(sam_out);
		else sam_close(bam_out);
	}
	if (bVCFoutput)
	{
		for (i = 0; i < iThreadNum; i++) pthread_create(&ThreadArr[i], NULL, CheckMappingCoverage, &ThrIdArr[i]);
		for (i = 0; i < iThreadNum; i++) pthread_join(ThreadArr[i], NULL);

		avgCov = (int)(1.0*iTotalCoverage / iAlignedBase + .5); if (avgCov < 0) avgCov = 0;
		fprintf(log, "\tEstimated AvgCoverage = %d\n", avgCov);
		fprintf(stderr, "\tEstimated AvgCoverage = %d\n", avgCov);
	}
	if (bVCFoutput)
	{
		pair<int64_t, int64_t> stat = ReportDuplicationRate();
		fprintf(log, "\tDuplication rate=%4.2f%%\n", 100 * (1.0*stat.first / stat.second));
		fprintf(stderr, "\tDuplication rate=%4.2f%%\n", 100 * (1.0*stat.first / stat.second));
	}
	if (iTotalReadNum > 0 && iTotalPairedNum > 0)
	{
		avgDist = (int)(1.*TotalPairedDistance / iTotalPairedNum + .5);
		avgReadLength = (int)(1.*ReadLengthSum / (iTotalPairedNum << 1) + .5);
		FragmentSize = avgDist + avgReadLength;
		fprintf(log, "\tAverage read length = %d, Estimated fragment size = %d, insert size = %d\n", avgReadLength, FragmentSize, avgDist - avgReadLength);
		fprintf(stderr, "\tAverage read length = %d, Estimated fragment size = %d, insert size = %d\n", avgReadLength, FragmentSize, avgDist - avgReadLength);
	}
	else avgDist = avgReadLength = 0;

	fclose(log);
	//if (ObserveBegPos != -1 && ObserveEndPos != -1)
	//{
	//	for (map<int64_t, map<string, uint16_t> >::iterator iter = InsertSeqMap.begin(); iter != InsertSeqMap.end(); iter++)
	//	{
	//		if (iter->first > ObserveBegPos && iter->first < ObserveEndPos)
	//		{
	//			for (map<string, uint16_t>::iterator SeqMapIter = iter->second.begin(); SeqMapIter != iter->second.end(); SeqMapIter++)
	//				if (SeqMapIter->second >= MinIndFreq) fprintf(stdout, "INS:%lld	%d	[%s] %d\n", (long long)iter->first, SeqMapIter->second, (char*)SeqMapIter->first.c_str(), (int)SeqMapIter->first.length());
	//		}
	//	}
	//	for (map<int64_t, map<string, uint16_t> >::iterator iter = DeleteSeqMap.begin(); iter != DeleteSeqMap.end(); iter++)
	//	{
	//		if (iter->first > ObserveBegPos && iter->first < ObserveEndPos)
	//		{
	//			for (map<string, uint16_t>::iterator SeqMapIter = iter->second.begin(); SeqMapIter != iter->second.end(); SeqMapIter++)
	//				if (SeqMapIter->second >= MinIndFreq) fprintf(stdout, "DEL:%lld	%d	[%s] %d\n", (long long)iter->first, SeqMapIter->second, (char*)SeqMapIter->first.c_str(), (int)SeqMapIter->first.length());
	//		}
	//	}
	//}
	delete[] ThrIdArr; delete[] ThreadArr;
}
