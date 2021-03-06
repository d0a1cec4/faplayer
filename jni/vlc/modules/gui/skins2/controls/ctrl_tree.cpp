/*****************************************************************************
 * ctrl_tree.cpp
 *****************************************************************************
 * Copyright (C) 2003 the VideoLAN team
 * $Id$
 *
 * Authors: Antoine Cellerier <dionoea@videolan.org>
 *          Clément Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <math.h>
#include "../utils/var_bool.hpp"
#include "ctrl_tree.hpp"
#include "../src/os_factory.hpp"
#include "../src/os_graphics.hpp"
#include "../src/generic_bitmap.hpp"
#include "../src/generic_font.hpp"
#include "../src/scaled_bitmap.hpp"
#include "../utils/position.hpp"
#include "../utils/ustring.hpp"
#include "../events/evt_key.hpp"
#include "../events/evt_mouse.hpp"
#include "../events/evt_scroll.hpp"
#include <vlc_keys.h>

#define SCROLL_STEP 0.05
#define LINE_INTERVAL 1  // Number of pixels inserted between 2 lines


CtrlTree::CtrlTree( intf_thread_t *pIntf,
                    VarTree &rTree,
                    const GenericFont &rFont,
                    const GenericBitmap *pBgBitmap,
                    const GenericBitmap *pItemBitmap,
                    const GenericBitmap *pOpenBitmap,
                    const GenericBitmap *pClosedBitmap,
                    uint32_t fgColor,
                    uint32_t playColor,
                    uint32_t bgColor1,
                    uint32_t bgColor2,
                    uint32_t selColor,
                    const UString &rHelp,
                    VarBool *pVisible,
                    VarBool *pFlat ):
    CtrlGeneric( pIntf,rHelp, pVisible), m_rTree( rTree), m_rFont( rFont ),
    m_pBgBitmap( pBgBitmap ), m_pItemBitmap( pItemBitmap ),
    m_pOpenBitmap( pOpenBitmap ), m_pClosedBitmap( pClosedBitmap ),
    m_fgColor( fgColor ), m_playColor( playColor ), m_bgColor1( bgColor1 ),
    m_bgColor2( bgColor2 ), m_selColor( selColor ),
    m_pLastSelected( NULL ), m_pImage( NULL ), m_dontMove( false ),
    m_pScaledBitmap( NULL )
{
    // Observe the tree and position variables
    m_rTree.addObserver( this );
    m_rTree.getPositionVar().addObserver( this );

    m_flat = pFlat->get();

    m_firstPos = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();

    makeImage();
}

CtrlTree::~CtrlTree()
{
    m_rTree.getPositionVar().delObserver( this );
    m_rTree.delObserver( this );
    delete m_pScaledBitmap;
    delete m_pImage;
}

int CtrlTree::itemHeight()
{
    int itemHeight = m_rFont.getSize();
    if( !m_flat )
    {
        if( m_pClosedBitmap )
        {
            itemHeight = __MAX( m_pClosedBitmap->getHeight(), itemHeight );
        }
        if( m_pOpenBitmap )
        {
            itemHeight = __MAX( m_pOpenBitmap->getHeight(), itemHeight );
        }
    }
    if( m_pItemBitmap )
    {
        itemHeight = __MAX( m_pItemBitmap->getHeight(), itemHeight );
    }
    itemHeight += LINE_INTERVAL;
    return itemHeight;
}

int CtrlTree::itemImageWidth()
{
    int bitmapWidth = 5;
    if( !m_flat )
    {
        if( m_pClosedBitmap )
        {
            bitmapWidth = __MAX( m_pClosedBitmap->getWidth(), bitmapWidth );
        }
        if( m_pOpenBitmap )
        {
            bitmapWidth = __MAX( m_pOpenBitmap->getWidth(), bitmapWidth );
        }
    }
    if( m_pItemBitmap )
    {
        bitmapWidth = __MAX( m_pItemBitmap->getWidth(), bitmapWidth );
    }
    return bitmapWidth + 2;
}

int CtrlTree::maxItems()
{
    const Position *pPos = getPosition();
    if( !pPos )
    {
        return -1;
    }
    return pPos->getHeight() / itemHeight();
}


void CtrlTree::onUpdate( Subject<VarTree, tree_update> &rTree,
                         tree_update *arg )
{
    if( arg->type == arg->UpdateItem ) // Item update
    {
        if( arg->b_active_item )
            autoScroll();
        if( isItemVisible( arg->i_id ) )
        {
            makeImage();
            notifyLayout();
        }
    }
    else if ( arg->type == arg->ResetAll ) // Global change or deletion
    {
        m_firstPos = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();

        makeImage();
        notifyLayout();
    }
    else if ( arg->type == arg->AppendItem ) // Item-append
    {
        if( m_flat && m_firstPos->size() )
        {
            m_firstPos = m_rTree.getNextLeaf( m_firstPos );

            makeImage();
            notifyLayout();
        }
        else if( isItemVisible( arg->i_id ) )
        {
            makeImage();
            notifyLayout();
        }
    }
    else if( arg->type == arg->DeleteItem ) // item-del
    {
        /* Make sure firstPos is valid */
        VarTree::Iterator it_old = m_firstPos;
        while( m_firstPos->isDeleted() &&
               m_firstPos != (m_flat ? m_rTree.firstLeaf()
                                     : m_rTree.begin()) )
        {
            m_firstPos = m_flat ? m_rTree.getPrevLeaf( m_firstPos )
                                : m_rTree.getPrevVisibleItem( m_firstPos );
        }
        if( m_firstPos->isDeleted() )
            m_firstPos = m_rTree.begin();

        if( m_firstPos != it_old || isItemVisible( arg->i_id ) )
        {
            makeImage();
            notifyLayout();
        }
    }
}

void CtrlTree::onUpdate( Subject<VarPercent> &rPercent, void* arg)
{
    // Determine what is the first item to display
    VarTree::Iterator it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();

    if( m_dontMove ) return;

    int excessItems;
    if( m_flat )
        excessItems = m_rTree.countLeafs() - maxItems();
    else
        excessItems = m_rTree.visibleItems() - maxItems();

    if( excessItems > 0)
    {
        VarPercent &rVarPos = m_rTree.getPositionVar();
        // a simple (int)(...) causes rounding errors !
#ifdef _MSC_VER
#   define lrint (int)
#endif
        if( m_flat )
            it = m_rTree.getLeaf(lrint( (1.0 - rVarPos.get()) * (double)excessItems ) + 1 );
        else
            it = m_rTree.getVisibleItem(lrint( (1.0 - rVarPos.get()) * (double)excessItems ) + 1 );
    }
    if( m_firstPos != it )
    {
        // Redraw the control if the position has changed
        m_firstPos = it;
        makeImage();
        notifyLayout();
    }
}

void CtrlTree::onResize()
{
    // Determine what is the first item to display
    VarTree::Iterator it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();

    int excessItems;
    if( m_flat )
        excessItems = m_rTree.countLeafs() - maxItems();
    else
        excessItems = m_rTree.visibleItems() - maxItems();

    if( excessItems > 0)
    {
        VarPercent &rVarPos = m_rTree.getPositionVar();
        // a simple (int)(...) causes rounding errors !
#ifdef _MSC_VER
#   define lrint (int)
#endif
        if( m_flat )
            it = m_rTree.getLeaf(lrint( (1.0 - rVarPos.get()) * (double)excessItems ) + 1 );
        else
            it = m_rTree.getVisibleItem(lrint( (1.0 - rVarPos.get()) * (double)excessItems ) + 1 );
    }
    // Redraw the control if the position has changed
    m_firstPos = it;
    makeImage();
}

void CtrlTree::onPositionChange()
{
    makeImage();
}

void CtrlTree::handleEvent( EvtGeneric &rEvent )
{
    bool bChangedPosition = false;
    VarTree::Iterator toShow; bool needShow = false;
    if( rEvent.getAsString().find( "key:down" ) != string::npos )
    {
        int key = ((EvtKey&)rEvent).getKey();
        VarTree::Iterator it;
        bool previousWasSelected = false;

        /* Delete the selection */
        if( key == KEY_DELETE )
        {
            /* Find first non selected item before m_pLastSelected */
            VarTree::Iterator it_sel = m_flat ? m_rTree.firstLeaf()
                                              : m_rTree.begin();
            for( it = (m_flat ? m_rTree.firstLeaf() : m_rTree.begin());
                 it != m_rTree.end();
                 it = (m_flat ? m_rTree.getNextLeaf( it )
                              : m_rTree.getNextVisibleItem( it )) )
            {
                if( &*it == m_pLastSelected ) break;
                if( !it->isSelected() ) it_sel = it;
            }

            /* Delete selected stuff */
            m_rTree.delSelected();

            /* Verify if there is still sthg selected (e.g read-only items) */
            m_pLastSelected = NULL;
            for( it = (m_flat ? m_rTree.firstLeaf() : m_rTree.begin());
                 it != m_rTree.end();
                 it = (m_flat ? m_rTree.getNextLeaf( it )
                              : m_rTree.getNextVisibleItem( it )) )
            {
                if( it->isSelected() )
                    m_pLastSelected = &*it;
            }

            /* if everything was deleted, use it_sel as last selection */
            if( !m_pLastSelected )
            {
                it_sel->setSelected( true );
                m_pLastSelected = &*it_sel;
            }

            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else if( key == KEY_PAGEDOWN )
        {
            it = m_firstPos;
            int i = (int)(maxItems()*1.5);
            while( i >= 0 )
            {
                VarTree::Iterator it_old = it;
                it = m_flat ? m_rTree.getNextLeaf( it )
                            : m_rTree.getNextVisibleItem( it );
                /* End is already visible, dont' scroll */
                if( it == m_rTree.end() )
                {
                    it = it_old;
                    break;
                }
                needShow = true;
                i--;
            }
            if( needShow )
            {
                ensureVisible( it );
                makeImage();
                notifyLayout();
            }
        }
        else if (key == KEY_PAGEUP )
        {
            it = m_firstPos;
            int i = maxItems();
            while( i >= maxItems()/2 )
            {
                it = m_flat ? m_rTree.getPrevLeaf( it )
                            : m_rTree.getPrevVisibleItem( it );
                /* End is already visible, dont' scroll */
                if( it == ( m_flat ? m_rTree.firstLeaf() : m_rTree.begin() ) )
                {
                    break;
                }
                i--;
            }
            ensureVisible( it );
            makeImage();
            notifyLayout();
        }
        else if ( key == KEY_UP ||
                  key == KEY_DOWN ||
                  key == KEY_LEFT ||
                  key == KEY_RIGHT ||
                  key == KEY_ENTER ||
                  key == ' ' )
        {
            for( it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
                 it != m_rTree.end();
                 it = m_flat ? m_rTree.getNextLeaf( it )
                             : m_rTree.getNextVisibleItem( it ) )
            {
                VarTree::Iterator next = m_flat ?
                                         m_rTree.getNextLeaf( it ) :
                                         m_rTree.getNextVisibleItem( it );
                if( key == KEY_UP )
                {
                    // Scroll up one item
                    if( ( it->parent()
                          && it != it->parent()->begin() )
                        || &*it != m_pLastSelected )
                    {
                        bool nextWasSelected = ( &*next == m_pLastSelected );
                        it->setSelected( nextWasSelected );
                        if( nextWasSelected )
                        {
                            m_pLastSelected = &*it;
                            needShow = true; toShow = it;
                        }
                    }
                }
                else if( key == KEY_DOWN )
                {
                    // Scroll down one item
                    if( ( it->parent()
                          && next != it->parent()->end() )
                        || &*it != m_pLastSelected )
                    {
                        it->setSelected( previousWasSelected );
                    }
                    if( previousWasSelected )
                    {
                        m_pLastSelected = &*it;
                        needShow = true; toShow = it;
                        previousWasSelected = false;
                    }
                    else
                    {
                        previousWasSelected = ( &*it == m_pLastSelected );
                    }

                    // Fix last tree item selection
                    if( ( m_flat ? m_rTree.getNextLeaf( it )
                        : m_rTree.getNextVisibleItem( it ) ) == m_rTree.end()
                     && &*it == m_pLastSelected )
                    {
                        it->setSelected( true );
                    }
                }
                else if( key == KEY_RIGHT )
                {
                    // Go down one level (and expand node)
                    if( &*it == m_pLastSelected )
                    {
                        if( it->isExpanded() )
                        {
                            if( it->size() )
                            {
                                it->setSelected( false );
                                it->begin()->setSelected( true );
                                m_pLastSelected = &*(it->begin());
                            }
                            else
                            {
                                m_rTree.action( &*it );
                            }
                        }
                        else
                        {
                            it->setExpanded( true );
                            bChangedPosition = true;
                        }
                    }
                }
                else if( key == KEY_LEFT )
                {
                    // Go up one level (and close node)
                    if( &*it == m_pLastSelected )
                    {
                        if( it->isExpanded() && it->size() )
                        {
                            it->setExpanded( false );
                            bChangedPosition = true;
                        }
                        else
                        {
                            if( it->parent() && it->parent() != &m_rTree)
                            {
                                it->setSelected( false );
                                m_pLastSelected = it->parent();
                                m_pLastSelected->setSelected( true );
                            }
                        }
                    }
                }
                else if( key == KEY_ENTER || key == ' ' )
                {
                    // Go up one level (and close node)
                    if( &*it == m_pLastSelected )
                    {
                        m_rTree.action( &*it );
                    }
                }
            }
            if( needShow )
                ensureVisible( toShow );
            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else
        {
            // other keys to be forwarded to vlc core
            EvtKey& rEvtKey = (EvtKey&)rEvent;
            var_SetInteger( getIntf()->p_libvlc, "key-pressed",
                            rEvtKey.getModKey() );
        }

    }

    else if( rEvent.getAsString().find( "mouse:left" ) != string::npos )
    {
        EvtMouse &rEvtMouse = (EvtMouse&)rEvent;
        const Position *pos = getPosition();
        int yPos = ( rEvtMouse.getYPos() - pos->getTop() ) / itemHeight();
        int xPos = rEvtMouse.getXPos() - pos->getLeft();
        VarTree::Iterator it;

        if( rEvent.getAsString().find( "mouse:left:down:ctrl,shift" ) !=
            string::npos )
        {
            VarTree::Iterator itClicked = findItemAtPos( yPos );
            // Flag to know if the current item must be selected
            bool select = false;
            for( it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
                 it != m_rTree.end();
                 it = m_flat ? m_rTree.getNextLeaf( it )
                             : m_rTree.getNextVisibleItem( it ) )
            {
                bool nextSelect = select;
                if( it == itClicked || &*it == m_pLastSelected )
                {
                    if( select )
                    {
                        nextSelect = false;
                    }
                    else
                    {
                        select = true;
                        nextSelect = true;
                    }
                }
                it->setSelected( it->isSelected() || select );
                select = nextSelect;
            }
            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else if( rEvent.getAsString().find( "mouse:left:down:ctrl" ) !=
                 string::npos )
        {
            // Invert the selection of the item
            it = findItemAtPos( yPos );
            if( it != m_rTree.end() )
            {
                it->toggleSelected();
                m_pLastSelected = &*it;
            }
            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else if( rEvent.getAsString().find( "mouse:left:down:shift" ) !=
                 string::npos )
        {
            VarTree::Iterator itClicked = findItemAtPos( yPos );
            // Flag to know if the current item must be selected
            bool select = false;
            for( it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
                 it != m_rTree.end();
                 it = m_flat ? m_rTree.getNextLeaf( it )
                             : m_rTree.getNextVisibleItem( it ) )
            {
                bool nextSelect = select;
                if( it == itClicked || &*it == m_pLastSelected )
                {
                    if( select )
                    {
                        nextSelect = false;
                    }
                    else
                    {
                        select = true;
                        nextSelect = true;
                    }
                }
                it->setSelected( select );
                select = nextSelect;
            }
            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else if( rEvent.getAsString().find( "mouse:left:down" ) !=
                 string::npos )
        {
            it = findItemAtPos(yPos);
            if( it != m_rTree.end() )
            {
                if( ( it->size() && xPos > (it->depth() - 1) * itemImageWidth()
                      && xPos < it->depth() * itemImageWidth() )
                 && !m_flat )
                {
                    // Fold/unfold the item
                    it->toggleExpanded();
                    bChangedPosition = true;
                }
                else
                {
                    // Unselect any previously selected item
                    VarTree::Iterator it2;
                    for( it2 = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
                         it2 != m_rTree.end();
                         it2 = m_flat ? m_rTree.getNextLeaf( it2 )
                                      : m_rTree.getNextVisibleItem( it2 ) )
                    {
                        it2->setSelected( false );
                    }
                    // Select the new item
                    if( it != m_rTree.end() )
                    {
                        it->setSelected( true );
                        m_pLastSelected = &*it;
                    }
                }
            }
            // Redraw the control
            makeImage();
            notifyLayout();
        }
        else if( rEvent.getAsString().find( "mouse:left:dblclick" ) !=
                 string::npos )
        {
            it = findItemAtPos(yPos);
            if( it != m_rTree.end() )
            {
               // Execute the action associated to this item
               m_rTree.action( &*it );
            }
            // Redraw the control
            makeImage();
            notifyLayout();
        }
    }

    else if( rEvent.getAsString().find( "scroll" ) != string::npos )
    {
        // XXX ctrl_slider.cpp has two more (but slightly different)
        // XXX implementations of `scroll'. Figure out where it belongs.

        int direction = static_cast<EvtScroll&>(rEvent).getDirection();

        double percentage = m_rTree.getPositionVar().get();
        double step = 2.0 / (double)( m_flat ? m_rTree.countLeafs()
                                             : m_rTree.visibleItems() );
        if( direction == EvtScroll::kUp )
        {
            percentage += step;
        }
        else
        {
            percentage -= step;
        }
        m_rTree.getPositionVar().set( percentage );
    }

    /* We changed the nodes, let's fix the position var */
    if( bChangedPosition )
    {
        VarTree::Iterator it;
        int iFirst = 0;
        for( it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
             it != m_rTree.end();
             it = m_flat ? m_rTree.getNextLeaf( it )
                         : m_rTree.getNextVisibleItem( it ) )
        {
            if( it == m_firstPos )
                break;
            iFirst++;
        }

        int indexMax = ( m_flat ? m_rTree.countLeafs()
                                : m_rTree.visibleItems() ) - 1;
        float f_new = (float)iFirst / (float)indexMax;

        m_dontMove = true;
        m_rTree.getPositionVar().set( 1.0 - f_new );
        m_dontMove = false;
    }
}

bool CtrlTree::mouseOver( int x, int y ) const
{
    const Position *pPos = getPosition();
    return !pPos ? false :
        x >= 0 && x <= pPos->getWidth() && y >= 0 && y <= pPos->getHeight();
}

void CtrlTree::draw( OSGraphics &rImage, int xDest, int yDest, int w, int h)
{
    const Position *pPos = getPosition();
    rect region( pPos->getLeft(), pPos->getTop(),
                 pPos->getWidth(), pPos->getHeight() );
    rect clip( xDest, yDest, w, h );
    rect inter;

    if( rect::intersect( region, clip, &inter ) && m_pImage )
        rImage.drawGraphics( *m_pImage,
                      inter.x - pPos->getLeft(),
                      inter.y - pPos->getTop(),
                      inter.x, inter.y, inter.width, inter.height );
}

bool CtrlTree::ensureVisible( VarTree::Iterator item )
{
    m_rTree.ensureExpanded( item );

    int firstPosIndex = m_rTree.getRank( m_firstPos, m_flat) - 1;
    int focusItemIndex = m_rTree.getRank( item, m_flat) - 1;

    if( focusItemIndex < firstPosIndex ||
        focusItemIndex > firstPosIndex + maxItems() - 1 )
    {
        // Scroll to have the wanted stream visible
        VarPercent &rVarPos = m_rTree.getPositionVar();
        int indexMax = ( m_flat ? m_rTree.countLeafs()
                                : m_rTree.visibleItems() ) - 1;
        rVarPos.set( 1.0 - (double)focusItemIndex / (double)indexMax );
        return true;
    }
    return false;
}

void CtrlTree::autoScroll()
{
    // Find the current playing stream
    int playIndex = 0;
    VarTree::Iterator it;

    for( it = m_flat ? m_rTree.firstLeaf() : m_rTree.begin();
         it != m_rTree.end();
         it = m_flat ? m_rTree.getNextLeaf( it )
                     : m_rTree.getNextItem( it ) )
    {
        if( it->isPlaying() )
        {
           ensureVisible( it );
           break;
        }
    }
}


void CtrlTree::makeImage()
{
    stats_TimerStart( getIntf(), "[Skins] Playlist image",
                      STATS_TIMER_SKINS_PLAYTREE_IMAGE );
    delete m_pImage;

    // Get the size of the control
    const Position *pPos = getPosition();
    if( !pPos )
    {
        stats_TimerStop( getIntf(), STATS_TIMER_SKINS_PLAYTREE_IMAGE );
        return;
    }
    int width = pPos->getWidth();
    int height = pPos->getHeight();

    int i_itemHeight = itemHeight();

    // Create an image
    OSFactory *pOsFactory = OSFactory::instance( getIntf() );
    m_pImage = pOsFactory->createOSGraphics( width, height );

    VarTree::Iterator it = m_firstPos;

    if( m_pBgBitmap )
    {
        // Draw the background bitmap
        if( !m_pScaledBitmap ||
            m_pScaledBitmap->getWidth() != width ||
            m_pScaledBitmap->getHeight() != height )
        {
            delete m_pScaledBitmap;
            m_pScaledBitmap =
                new ScaledBitmap( getIntf(), *m_pBgBitmap, width, height );
        }
        m_pImage->drawBitmap( *m_pScaledBitmap, 0, 0 );

        for( int yPos = 0; yPos < height; yPos += i_itemHeight )
        {
            if( it != m_rTree.end() )
            {
                if( it->isSelected() )
                {
                    int rectHeight = __MIN( i_itemHeight, height - yPos );
                    m_pImage->fillRect( 0, yPos, width, rectHeight,
                                        m_selColor );
                }
                do
                {
                    it = m_flat ? m_rTree.getNextLeaf( it )
                                : m_rTree.getNextVisibleItem( it );
                } while( it != m_rTree.end() && it->isDeleted() );
            }
        }
    }
    else
    {
        // FIXME (TRYME)
        // Fill background with background color
        uint32_t bgColor = m_bgColor1;
        m_pImage->fillRect( 0, 0, width, height, bgColor );
        for( int yPos = 0; yPos < height; yPos += i_itemHeight )
        {
            int rectHeight = __MIN( i_itemHeight, height - yPos );
            if( it == m_rTree.end() )
                m_pImage->fillRect( 0, yPos, width, rectHeight, bgColor );
            else
            {
                uint32_t color = ( it->isSelected() ? m_selColor : bgColor );
                m_pImage->fillRect( 0, yPos, width, rectHeight, color );
                do
                {
                    it = m_flat ? m_rTree.getNextLeaf( it )
                                : m_rTree.getNextVisibleItem( it );
                } while( it != m_rTree.end() && it->isDeleted() );
            }
            bgColor = ( bgColor == m_bgColor1 ? m_bgColor2 : m_bgColor1 );
        }
    }

    int bitmapWidth = itemImageWidth();

    int yPos = 0;
    it = m_firstPos;
    while( it != m_rTree.end() && yPos < height )
    {
        const GenericBitmap *m_pCurBitmap;
        UString *pStr = it->getString();
        uint32_t color = ( it->isPlaying() ? m_playColor : m_fgColor );

        // Draw the text
        if( pStr != NULL )
        {
            int depth = m_flat ? 1 : it->depth();
            GenericBitmap *pText = m_rFont.drawString( *pStr, color, width - bitmapWidth * depth );
            if( !pText )
            {
                stats_TimerStop( getIntf(), STATS_TIMER_SKINS_PLAYTREE_IMAGE );
                return;
            }
            if( it->size() )
                m_pCurBitmap = it->isExpanded() ? m_pOpenBitmap : m_pClosedBitmap;
            else
                m_pCurBitmap = m_pItemBitmap;

            if( m_pCurBitmap )
            {
                // Make sure we are centered on the line
                int yPos2 = yPos+(i_itemHeight-m_pCurBitmap->getHeight()+1)/2;
                if( yPos2 >= height )
                {
                    delete pText;
                    break;
                }
                m_pImage->drawBitmap( *m_pCurBitmap, 0, 0,
                                      bitmapWidth * (depth - 1 ), yPos2,
                                      m_pCurBitmap->getWidth(),
                                      __MIN( m_pCurBitmap->getHeight(),
                                             height -  yPos2), true );
            }
            yPos += i_itemHeight - pText->getHeight();
            int ySrc = 0;
            if( yPos < 0 )
            {
                ySrc = - yPos;
                yPos = 0;
            }
            int lineHeight = __MIN( pText->getHeight() - ySrc, height - yPos );
            m_pImage->drawBitmap( *pText, 0, ySrc, bitmapWidth * depth, yPos,
                                  pText->getWidth(),
                                  lineHeight, true );
            yPos += (pText->getHeight() - ySrc );
            delete pText;
        }
        do
        {
            it = m_flat ? m_rTree.getNextLeaf( it )
                : m_rTree.getNextVisibleItem( it );
        } while( it != m_rTree.end() && it->isDeleted() );
    }
    stats_TimerStop( getIntf(), STATS_TIMER_SKINS_PLAYTREE_IMAGE );
}

VarTree::Iterator CtrlTree::findItemAtPos( int pos )
{
    // The first item is m_firstPos.
    // We decrement pos as we try the other items, until pos == 0.
    VarTree::Iterator it;
    for( it = m_firstPos; it != m_rTree.end() && pos != 0;
         it = m_flat ? m_rTree.getNextLeaf( it )
                     : m_rTree.getNextVisibleItem( it ) )
    {
        pos--;
    }

    return it;
}

bool CtrlTree::isItemVisible( int id )
{
    VarTree::Iterator it = m_rTree.findById( id );

    int rank1 = m_rTree.getRank( m_firstPos, m_flat );
    int rank2 = m_rTree.getRank( it, m_flat );
    return ( rank2 >= rank1 && rank2 <= rank1 + maxItems() -1 );
}
