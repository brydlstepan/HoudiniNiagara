/*
* Copyright (c) <2018> Side Effects Software Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*/

#include "HoudiniPointCacheLoader.h"

#include "HoudiniPointCache.h"

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"

FHoudiniPointCacheSortPredicate::FHoudiniPointCacheSortPredicate(const int32 &InTimeAttrIndex, const int32 &InAgeAttrIndex, const int32 &InIDAttrIndex )
    : TimeAttributeIndex( InTimeAttrIndex ), AgeAttributeIndex(InAgeAttrIndex), IDAttributeIndex( InIDAttrIndex )
{

}

bool FHoudiniPointCacheSortPredicate::operator()( const TArray<FString>& A, const TArray<FString>& B ) const
{
    float ATime = TNumericLimits< float >::Lowest();
    if ( A.IsValidIndex( TimeAttributeIndex ) )
        ATime = FCString::Atof( *A[ TimeAttributeIndex ] );

    float BTime = TNumericLimits< float >::Lowest();
    if ( B.IsValidIndex( TimeAttributeIndex ) )
        BTime = FCString::Atof( *B[ TimeAttributeIndex ] );

    if ( ATime != BTime )
    {
        return ATime < BTime;
    }
    else
    {
        float AAge = TNumericLimits< float >::Lowest();
        if (A.IsValidIndex(AgeAttributeIndex))
            AAge = FCString::Atof(*A[AgeAttributeIndex]);

        float BAge = TNumericLimits< float >::Lowest();
        if (B.IsValidIndex(AgeAttributeIndex))
            BAge = FCString::Atof(*B[AgeAttributeIndex]);

        if (AAge != BAge)
        {
            return BAge < AAge;
        }
        else
        {
            float AID = TNumericLimits< float >::Lowest();
            if (A.IsValidIndex(IDAttributeIndex))
                AID = FCString::Atof(*A[IDAttributeIndex]);

            float BID = TNumericLimits< float >::Lowest();
            if (B.IsValidIndex(IDAttributeIndex))
                BID = FCString::Atof(*B[IDAttributeIndex]);

            return AID <= BID;
        }
    }
}

FHoudiniPointCacheLoader::FHoudiniPointCacheLoader(const FString& InFilePath)
    : FilePath(InFilePath)
{

}

FHoudiniPointCacheLoader::~FHoudiniPointCacheLoader()
{
    
}

#if WITH_EDITOR
bool FHoudiniPointCacheLoader::LoadRawPointCacheData(UHoudiniPointCache* InAsset, const FString& InFilePath) const
{
    InAsset->Modify();
    
    // Load file into temporary array first
    TArray<uint8, FDefaultAllocator64> TempData;
    if (!FFileHelper::LoadFileToArray(TempData, *InFilePath))
    {
        return false;
    }

    // Copy to FByteBulkData
    InAsset->RawDataCompressed.Lock(LOCK_READ_WRITE);
    void* Dest = InAsset->RawDataCompressed.Realloc(TempData.Num());
    FMemory::Memcpy(Dest, TempData.GetData(), TempData.Num());
    InAsset->RawDataCompressed.Unlock();

    // Force the payload to be stored in a separate file (or at end of file) to avoid package size limits
    // Also enable 64-bit size/offset to support files > 2GB
    InAsset->RawDataCompressed.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload | BULKDATA_Size64Bit);

    return true;
}
#endif


#if WITH_EDITOR
void FHoudiniPointCacheLoader::CompressRawData(UHoudiniPointCache* InAsset) const
{
    constexpr ECompressionFlags CompressFlags = COMPRESS_BiasMemory;
    const int64 UncompressedSize = InAsset->RawDataCompressed.GetBulkDataSize();

    // Skip compression for files larger than 2GB due to internal FCompression limitations
    // Even though the API accepts int64, there are internal int32 checks that will fail
    if (UncompressedSize > INT32_MAX)
    {
        UE_LOG(LogHoudiniNiagara, Warning, TEXT("Skipping compression for large file (%lld bytes). File will be stored uncompressed."), UncompressedSize);
        InAsset->RawDataUncompressedSize = UncompressedSize;
        InAsset->RawDataCompressionMethod = NAME_None;
        InAsset->RawDataFormatID = GetFormatID();
        return;
    }

    const FName CompressionName = NAME_Oodle;
	int64 CompressedSize = FCompression::CompressMemoryBound(CompressionName, UncompressedSize, CompressFlags);
	TArray<uint8, FDefaultAllocator64> CompressedData;

    CompressedData.SetNum(CompressedSize);

    // Lock BulkData for reading
    const void* Src = InAsset->RawDataCompressed.LockReadOnly();

	if (FCompression::CompressMemory(
	    CompressionName,
	    CompressedData.GetData(),
	    CompressedSize,
	    Src,
	    UncompressedSize,
	    CompressFlags))
	{
        // Unlock read access
        InAsset->RawDataCompressed.Unlock();

		CompressedData.SetNum(CompressedSize);
		CompressedData.Shrink();
	    
        // Write compressed data back to BulkData
        InAsset->RawDataCompressed.Lock(LOCK_READ_WRITE);
        void* Dest = InAsset->RawDataCompressed.Realloc(CompressedData.Num());
        FMemory::Memcpy(Dest, CompressedData.GetData(), CompressedData.Num());
        InAsset->RawDataCompressed.Unlock();

        // consistently force separate storage and 64-bit size
        InAsset->RawDataCompressed.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload | BULKDATA_Size64Bit);

	    InAsset->RawDataCompressionMethod = CompressionName;
	}
    else
    {
        // Unlock read access if compression failed
        InAsset->RawDataCompressed.Unlock();
    }
    
    InAsset->RawDataUncompressedSize = UncompressedSize;
    InAsset->RawDataFormatID = GetFormatID();
}
#endif

#if WITH_EDITOR
bool FHoudiniPointCacheLoader::GetUncompressedRawData(const UHoudiniPointCache* InAsset, TArray<uint8, FDefaultAllocator64>& OutData) const
{
    if (!InAsset || !InAsset->HasRawData())
        return false;

    // Handle uncompressed data (for files >2GB that skip compression)
    if (InAsset->RawDataCompressionMethod.IsEqual(NAME_None))
    {
        // Data is uncompressed, copy directly
        const void* BulkDataPtr = InAsset->RawDataCompressed.LockReadOnly();
        int64 BulkDataSize = InAsset->RawDataCompressed.GetBulkDataSize();
        
        OutData.SetNumUninitialized(BulkDataSize);
        FMemory::Memcpy(OutData.GetData(), BulkDataPtr, BulkDataSize);
        
        InAsset->RawDataCompressed.Unlock();
        return true;
    }

    // Uncompress data
    const int64 UncompressedSize = InAsset->RawDataUncompressedSize;
    int64 CompressedSize = InAsset->RawDataCompressed.GetBulkDataSize();
    
    OutData.SetNumUninitialized(UncompressedSize);
    
    const void* BulkDataPtr = InAsset->RawDataCompressed.LockReadOnly();

    bool bSuccess = FCompression::UncompressMemory(
        InAsset->RawDataCompressionMethod,
        OutData.GetData(),
        UncompressedSize,
        BulkDataPtr,
        CompressedSize);
    
    InAsset->RawDataCompressed.Unlock();
    
    return bSuccess;
}
#endif
