from fastapi import APIRouter, Depends, HTTPException

from models.processing_memory import UpdateProcessingMemoryResponse, UpdateProcessingMemory
import utils.processing_memories as processing_memory_utils
from utils.other import endpoints as auth

router = APIRouter()

@router.patch("/v1/processing-memories/{processing_memory_id}", response_model=UpdateProcessingMemoryResponse, tags=['processing_memories'])
def update_processing_memory(
        processing_memory_id: str,
        update_processing_memory: UpdateProcessingMemory,
        uid: str = Depends(auth.get_current_user_uid)
):
    """
    Update ProcessingMemory endpoint.
    :param update_processing_memory: data to update processing_memory
    :param uid: user id.
    :return: The new processing_memory updated.
    """

    print(f"Update processing memory {processing_memory_id}")

    update_processing_memory.id = processing_memory_id
    processing_memory = processing_memory_utils.update_processing_memory(uid, update_processing_memory)
    if not processing_memory:
        raise HTTPException(status_code=404, detail="Processing memory not found")

    return UpdateProcessingMemoryResponse(result=UpdateProcessingMemory(
        id=processing_memory.id,
        emotional_feedback=processing_memory.emotional_feedback,
        geolocation=processing_memory.geolocation,
    ))
